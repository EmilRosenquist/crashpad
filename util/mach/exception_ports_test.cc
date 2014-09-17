// Copyright 2014 The Crashpad Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "util/mach/exception_ports.h"

#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include "base/basictypes.h"
#include "base/logging.h"
#include "base/mac/mach_logging.h"
#include "base/mac/scoped_mach_port.h"
#include "base/strings/stringprintf.h"
#include "gtest/gtest.h"
#include "util/file/fd_io.h"
#include "util/mach/exc_server_variants.h"
#include "util/mach/mach_extensions.h"
#include "util/misc/scoped_forbid_return.h"
#include "util/test/errors.h"
#include "util/test/mac/mach_errors.h"
#include "util/test/mac/mach_multiprocess.h"

namespace {

using namespace crashpad;
using namespace crashpad::test;

// Calls GetExceptionPorts() on its |exception_ports| argument to look up the
// EXC_MASK_CRASH handler. If |expect_port| is not MACH_PORT_NULL, it expects to
// find a handler for this mask whose port matches |expect_port| and whose
// behavior matches |expect_behavior| exactly. In this case, if
// |expect_behavior| is a state-carrying behavior, the looked-up thread state
// flavor is expected to be MACHINE_THREAD_STATE, otherwise, it is expected to
// be THREAD_STATE_NONE. If |expect_port| is MACH_PORT_NULL, no handler for
// EXC_MASK_CRASH is expected to be found.
//
// A second GetExceptionPorts() lookup is also performed on a wider exception
// mask, EXC_MASK_ALL | EXC_MASK_CRASH. The EXC_MASK_CRASH handler’s existence
// and properties from this second lookup are validated in the same way.
//
// This function uses gtest EXPECT_* and ASSERT_* macros to perform its
// validation.
void TestGetExceptionPorts(const ExceptionPorts& exception_ports,
                           mach_port_t expect_port,
                           exception_behavior_t expect_behavior) {
  const exception_mask_t kExceptionMask = EXC_MASK_CRASH;

  thread_state_flavor_t expect_flavor = (expect_behavior == EXCEPTION_DEFAULT)
                                            ? THREAD_STATE_NONE
                                            : MACHINE_THREAD_STATE;

  std::vector<ExceptionPorts::ExceptionHandler> crash_handler;
  ASSERT_TRUE(
      exception_ports.GetExceptionPorts(kExceptionMask, &crash_handler));

  if (expect_port != MACH_PORT_NULL) {
    ASSERT_EQ(1u, crash_handler.size());
    base::mac::ScopedMachSendRight port_owner(crash_handler[0].port);

    EXPECT_EQ(kExceptionMask, crash_handler[0].mask);
    EXPECT_EQ(expect_port, crash_handler[0].port);
    EXPECT_EQ(expect_behavior, crash_handler[0].behavior);
    EXPECT_EQ(expect_flavor, crash_handler[0].flavor);
  } else {
    EXPECT_TRUE(crash_handler.empty());
  }

  std::vector<ExceptionPorts::ExceptionHandler> handlers;
  ASSERT_TRUE(exception_ports.GetExceptionPorts(
      ExcMaskAll() | EXC_MASK_CRASH, &handlers));

  EXPECT_GE(handlers.size(), crash_handler.size());
  bool found = false;
  for (const ExceptionPorts::ExceptionHandler& handler : handlers) {
    if ((handler.mask & kExceptionMask) != 0) {
      base::mac::ScopedMachSendRight port_owner(handler.port);

      EXPECT_FALSE(found);
      found = true;
      EXPECT_EQ(expect_port, handler.port);
      EXPECT_EQ(expect_behavior, handler.behavior);
      EXPECT_EQ(expect_flavor, handler.flavor);
    }
  }

  if (expect_port != MACH_PORT_NULL) {
    EXPECT_TRUE(found);
  } else {
    EXPECT_FALSE(found);
  }
}

class TestExceptionPorts : public UniversalMachExcServer,
                           public MachMultiprocess {
 public:
  // Where to call ExceptionPorts::SetExceptionPort() from.
  enum SetType {
    // Call it from the child process on itself.
    kSetInProcess = 0,

    // Call it from the parent process on the child.
    kSetOutOfProcess,
  };

  // Which entities to set exception ports for.
  enum SetOn {
    kSetOnTaskOnly = 0,
    kSetOnTaskAndThreads,
  };

  // Which thread in the child process is expected to crash.
  enum WhoCrashes {
    kNobodyCrashes = 0,
    kMainThreadCrashes,
    kOtherThreadCrashes,
  };

  TestExceptionPorts(SetType set_type, SetOn set_on, WhoCrashes who_crashes)
      : UniversalMachExcServer(),
        MachMultiprocess(),
        set_type_(set_type),
        set_on_(set_on),
        who_crashes_(who_crashes),
        handled_(false) {}

  SetType set_type() const { return set_type_; }
  SetOn set_on() const { return set_on_; }
  WhoCrashes who_crashes() const { return who_crashes_; }

  // UniversalMachExcServer:

  virtual kern_return_t CatchMachException(
      exception_behavior_t behavior,
      exception_handler_t exception_port,
      thread_t thread,
      task_t task,
      exception_type_t exception,
      const mach_exception_data_type_t* code,
      mach_msg_type_number_t code_count,
      thread_state_flavor_t* flavor,
      const natural_t* old_state,
      mach_msg_type_number_t old_state_count,
      thread_state_t new_state,
      mach_msg_type_number_t* new_state_count,
      bool* destroy_complex_request) override {
    *destroy_complex_request = true;

    EXPECT_FALSE(handled_);
    handled_ = true;

    // To be able to distinguish between which handler was actually triggered,
    // the different handlers are registered with different behavior values.
    exception_behavior_t expect_behavior;
    if (set_on_ == kSetOnTaskOnly) {
      expect_behavior = EXCEPTION_DEFAULT;
    } else if (who_crashes_ == kMainThreadCrashes) {
      expect_behavior = EXCEPTION_STATE;
    } else if (who_crashes_ == kOtherThreadCrashes) {
      expect_behavior = EXCEPTION_STATE_IDENTITY;
    } else {
      NOTREACHED();
      expect_behavior = 0;
    }

    EXPECT_EQ(expect_behavior, behavior);

    EXPECT_EQ(LocalPort(), exception_port);

    EXPECT_EQ(EXC_CRASH, exception);
    EXPECT_EQ(2u, code_count);

    // The exception and code_count checks above would ideally use ASSERT_EQ so
    // that the next conditional would not be necessary, but ASSERT_* requires a
    // function returning type void, and the interface dictates otherwise here.
    if (exception == EXC_CRASH && code_count >= 1) {
      int signal;
      ExcCrashRecoverOriginalException(code[0], NULL, &signal);

      // The child crashed with a division by zero, which shows up as SIGFPE.
      // This was chosen because it’s unlikely to be generated by testing or
      // assertion failures.
      EXPECT_EQ(SIGFPE, signal);

      SetExpectedChildTermination(kTerminationSignal, signal);
    }

    // Even for an EXC_CRASH handler, returning KERN_SUCCESS with a
    // state-carrying reply will cause the kernel to try to set a new thread
    // state, leading to a perceptible waste of time. Returning
    // MACH_RCV_PORT_DIED is the only way to suppress this behavior while also
    // preventing the kernel from looking for another (host-level) EXC_CRASH
    // handler. See 10.9.4 xnu-2422.110.17/osfmk/kern/exception.c
    // exception_triage().
    exception_behavior_t basic_behavior = behavior & ~MACH_EXCEPTION_CODES;
    bool has_state = basic_behavior == EXCEPTION_STATE ||
                     basic_behavior == EXCEPTION_STATE_IDENTITY;
    return has_state ? MACH_RCV_PORT_DIED : KERN_SUCCESS;
  }

 private:
  class Child {
   public:
    explicit Child(TestExceptionPorts* test_exception_ports)
        : test_exception_ports_(test_exception_ports),
          thread_(),
          init_semaphore_(dispatch_semaphore_create(0)),
          crash_semaphore_(dispatch_semaphore_create(0)) {}

    ~Child() {
      dispatch_release(crash_semaphore_);
      dispatch_release(init_semaphore_);
    }

    void Run() {
      ExceptionPorts self_task_ports(ExceptionPorts::kTargetTypeTask,
                                     MACH_PORT_NULL);
      ExceptionPorts self_thread_ports(ExceptionPorts::kTargetTypeThread,
                                       MACH_PORT_NULL);

      mach_port_t remote_port = test_exception_ports_->RemotePort();

      // Set the task’s and this thread’s exception ports, if appropriate.
      if (test_exception_ports_->set_type() == kSetInProcess) {
        ASSERT_TRUE(self_task_ports.SetExceptionPort(
            EXC_MASK_CRASH, remote_port, EXCEPTION_DEFAULT, THREAD_STATE_NONE));

        if (test_exception_ports_->set_on() == kSetOnTaskAndThreads) {
          ASSERT_TRUE(self_thread_ports.SetExceptionPort(EXC_MASK_CRASH,
                                                         remote_port,
                                                         EXCEPTION_STATE,
                                                         MACHINE_THREAD_STATE));
        }
      }

      int rv_int = pthread_create(&thread_, NULL, ThreadMainThunk, this);
      ASSERT_EQ(0, rv_int);

      // Wait for the new thread to be ready.
      long rv_long =
          dispatch_semaphore_wait(init_semaphore_, DISPATCH_TIME_FOREVER);
      ASSERT_EQ(0, rv_long);

      // Tell the parent process that everything is set up.
      char c = '\0';
      ssize_t rv_ssize = WriteFD(test_exception_ports_->WritePipeFD(), &c, 1);
      ASSERT_EQ(1, rv_ssize) << ErrnoMessage("write");

      // Wait for the parent process to say that its end is set up.
      rv_ssize = ReadFD(test_exception_ports_->ReadPipeFD(), &c, 1);
      ASSERT_EQ(1, rv_ssize) << ErrnoMessage("read");
      EXPECT_EQ('\0', c);

      // Regardless of where ExceptionPorts::SetExceptionPort() ran,
      // ExceptionPorts::GetExceptionPorts() can always be tested in-process.
      {
        SCOPED_TRACE("task");
        TestGetExceptionPorts(self_task_ports, remote_port, EXCEPTION_DEFAULT);
      }

      {
        SCOPED_TRACE("main_thread");
        mach_port_t thread_handler =
            (test_exception_ports_->set_on() == kSetOnTaskAndThreads)
                ? remote_port
                : MACH_PORT_NULL;
        TestGetExceptionPorts(
            self_thread_ports, thread_handler, EXCEPTION_STATE);
      }

      // Let the other thread know it’s safe to proceed.
      dispatch_semaphore_signal(crash_semaphore_);

      // If this thread is the one that crashes, do it.
      if (test_exception_ports_->who_crashes() == kMainThreadCrashes) {
        Crash();
      }

      // Reap the other thread.
      rv_int = pthread_join(thread_, NULL);
      ASSERT_EQ(0, rv_int);
    }

   private:
    // Calls ThreadMain().
    static void* ThreadMainThunk(void* argument) {
      Child* self = reinterpret_cast<Child*>(argument);
      return self->ThreadMain();
    }

    // Runs the “other” thread.
    void* ThreadMain() {
      ExceptionPorts self_thread_ports(ExceptionPorts::kTargetTypeThread,
                                       MACH_PORT_NULL);
      mach_port_t remote_port = test_exception_ports_->RemotePort();

      // Set this thread’s exception handler, if appropriate.
      if (test_exception_ports_->set_type() == kSetInProcess &&
          test_exception_ports_->set_on() == kSetOnTaskAndThreads) {
        CHECK(self_thread_ports.SetExceptionPort(EXC_MASK_CRASH,
                                                 remote_port,
                                                 EXCEPTION_STATE_IDENTITY,
                                                 MACHINE_THREAD_STATE));
      }

      // Let the main thread know that this thread is ready.
      dispatch_semaphore_signal(init_semaphore_);

      // Wait for the main thread to signal that it’s safe to proceed.
      long rv =
          dispatch_semaphore_wait(crash_semaphore_, DISPATCH_TIME_FOREVER);
      CHECK_EQ(0, rv) << "dispatch_semaphore_wait";

      // Regardless of where ExceptionPorts::SetExceptionPort() ran,
      // ExceptionPorts::GetExceptionPorts() can always be tested in-process.
      {
        SCOPED_TRACE("other_thread");
        mach_port_t thread_handler =
            (test_exception_ports_->set_on() == kSetOnTaskAndThreads)
                ? remote_port
                : MACH_PORT_NULL;
        TestGetExceptionPorts(
            self_thread_ports, thread_handler, EXCEPTION_STATE_IDENTITY);
      }

      // If this thread is the one that crashes, do it.
      if (test_exception_ports_->who_crashes() == kOtherThreadCrashes) {
        Crash();
      }

      return NULL;
    }

    // Crashes by performing a division by zero. The assignment is present to
    // avoid optimizing zero_ out entirely by making it appear that its value
    // might change.
    static void Crash() { zero_ = 1 / zero_; }

    // The parent object.
    TestExceptionPorts* test_exception_ports_;  // weak

    // The “other” thread.
    pthread_t thread_;

    // The main thread waits on this for the other thread to start up and
    // perform its own initialization.
    dispatch_semaphore_t init_semaphore_;

    // The child thread waits on this for the parent thread to indicate that the
    // child can test its exception ports and possibly crash, as appropriate.
    dispatch_semaphore_t crash_semaphore_;

    // Always zero. Crash() divides by this in order to trigger a crash. This is
    // structured as a static volatile int to ward off aggressive compiler
    // optimizations.
    static volatile int zero_;

    DISALLOW_COPY_AND_ASSIGN(Child);
  };

  // MachMultiprocess:

  virtual void MachMultiprocessParent() override {
    // Wait for the child process to be ready. It needs to have all of its
    // threads set up before proceeding if in kSetOutOfProcess mode.
    char c;
    ssize_t rv = ReadFD(ReadPipeFD(), &c, 1);
    ASSERT_EQ(1, rv) << ErrnoMessage("read");
    EXPECT_EQ('\0', c);

    mach_port_t local_port = LocalPort();

    // Get an ExceptionPorts object for the task and each of its threads.
    ExceptionPorts task_ports(ExceptionPorts::kTargetTypeTask, ChildTask());
    EXPECT_EQ("task", task_ports.TargetTypeName());

    // Hopefully the threads returned by task_threads() are in order, with the
    // main thread first and the other thread second. This is currently always
    // the case, although nothing guarantees that it will remain so.
    thread_act_array_t threads;
    mach_msg_type_number_t thread_count = 0;
    kern_return_t kr = task_threads(ChildTask(), &threads, &thread_count);
    ASSERT_EQ(KERN_SUCCESS, kr) << MachErrorMessage(kr, "task_threads");

    ScopedForbidReturn threads_need_owners;
    ASSERT_EQ(2u, thread_count);
    base::mac::ScopedMachSendRight main_thread(threads[0]);
    base::mac::ScopedMachSendRight other_thread(threads[1]);
    threads_need_owners.Disarm();

    ExceptionPorts main_thread_ports(ExceptionPorts::kTargetTypeThread,
                                     main_thread);
    ExceptionPorts other_thread_ports(ExceptionPorts::kTargetTypeThread,
                                      other_thread);
    EXPECT_EQ("thread", main_thread_ports.TargetTypeName());
    EXPECT_EQ("thread", other_thread_ports.TargetTypeName());

    if (set_type_ == kSetOutOfProcess) {
      // Test ExceptionPorts::SetExceptionPorts() being called from
      // out-of-process.
      //
      // local_port is only a receive right, but a send right is needed for
      // ExceptionPorts::SetExceptionPort(). Make a send right, which can be
      // deallocated once the calls to ExceptionPorts::SetExceptionPort() are
      // done.
      kr = mach_port_insert_right(
          mach_task_self(), local_port, local_port, MACH_MSG_TYPE_MAKE_SEND);
      ASSERT_EQ(KERN_SUCCESS, kr)
          << MachErrorMessage(kr, "mach_port_insert_right");
      base::mac::ScopedMachSendRight send_owner(local_port);

      ASSERT_TRUE(task_ports.SetExceptionPort(
          EXC_MASK_CRASH, local_port, EXCEPTION_DEFAULT, THREAD_STATE_NONE));

      if (set_on_ == kSetOnTaskAndThreads) {
        ASSERT_TRUE(main_thread_ports.SetExceptionPort(
            EXC_MASK_CRASH, local_port, EXCEPTION_STATE, MACHINE_THREAD_STATE));

        ASSERT_TRUE(
            other_thread_ports.SetExceptionPort(EXC_MASK_CRASH,
                                                local_port,
                                                EXCEPTION_STATE_IDENTITY,
                                                MACHINE_THREAD_STATE));
      }
    }

    // Regardless of where ExceptionPorts::SetExceptionPort() ran,
    // ExceptionPorts::GetExceptionPorts() can always be tested out-of-process.
    {
      SCOPED_TRACE("task");
      TestGetExceptionPorts(task_ports, local_port, EXCEPTION_DEFAULT);
    }

    mach_port_t thread_handler =
        (set_on_ == kSetOnTaskAndThreads) ? local_port : MACH_PORT_NULL;

    {
      SCOPED_TRACE("main_thread");
      TestGetExceptionPorts(main_thread_ports, thread_handler, EXCEPTION_STATE);
    }

    {
      SCOPED_TRACE("other_thread");
      TestGetExceptionPorts(
          other_thread_ports, thread_handler, EXCEPTION_STATE_IDENTITY);
    }

    // Let the child process know that everything in the parent process is set
    // up.
    c = '\0';
    rv = WriteFD(WritePipeFD(), &c, 1);
    ASSERT_EQ(1, rv) << ErrnoMessage("write");

    if (who_crashes_ != kNobodyCrashes) {
      kern_return_t kr = MachMessageServer::Run(this,
                                                local_port,
                                                MACH_MSG_OPTION_NONE,
                                                MachMessageServer::kOneShot,
                                                MachMessageServer::kBlocking,
                                                0);
      EXPECT_EQ(KERN_SUCCESS, kr)
          << MachErrorMessage(kr, "MachMessageServer::Run");

      EXPECT_TRUE(handled_);
    }

    // Wait for the child process to exit or terminate, as indicated by it
    // closing its pipe. This keeps LocalPort() alive in the child as
    // RemotePort(), for the child’s use in its TestGetExceptionPorts().
    rv = ReadFD(ReadPipeFD(), &c, 1);
    ASSERT_EQ(0, rv);
  }

  virtual void MachMultiprocessChild() override {
    Child child(this);
    child.Run();
  }

  SetType set_type_;
  SetOn set_on_;
  WhoCrashes who_crashes_;

  // true if an exception message was handled.
  bool handled_;

  DISALLOW_COPY_AND_ASSIGN(TestExceptionPorts);
};

volatile int TestExceptionPorts::Child::zero_ = 0;

TEST(ExceptionPorts, TaskAndThreadExceptionPorts) {
  struct Testcase {
    TestExceptionPorts::SetType set_type;
    TestExceptionPorts::SetOn set_on;
    TestExceptionPorts::WhoCrashes who_crashes;
  };
  const Testcase kTestcases[] = {
      {TestExceptionPorts::kSetInProcess,
       TestExceptionPorts::kSetOnTaskOnly,
       TestExceptionPorts::kNobodyCrashes},
      {TestExceptionPorts::kSetInProcess,
       TestExceptionPorts::kSetOnTaskOnly,
       TestExceptionPorts::kMainThreadCrashes},
      {TestExceptionPorts::kSetInProcess,
       TestExceptionPorts::kSetOnTaskOnly,
       TestExceptionPorts::kOtherThreadCrashes},
      {TestExceptionPorts::kSetInProcess,
       TestExceptionPorts::kSetOnTaskAndThreads,
       TestExceptionPorts::kNobodyCrashes},
      {TestExceptionPorts::kSetInProcess,
       TestExceptionPorts::kSetOnTaskAndThreads,
       TestExceptionPorts::kMainThreadCrashes},
      {TestExceptionPorts::kSetInProcess,
       TestExceptionPorts::kSetOnTaskAndThreads,
       TestExceptionPorts::kOtherThreadCrashes},
      {TestExceptionPorts::kSetOutOfProcess,
       TestExceptionPorts::kSetOnTaskOnly,
       TestExceptionPorts::kNobodyCrashes},
      {TestExceptionPorts::kSetOutOfProcess,
       TestExceptionPorts::kSetOnTaskOnly,
       TestExceptionPorts::kMainThreadCrashes},
      {TestExceptionPorts::kSetOutOfProcess,
       TestExceptionPorts::kSetOnTaskOnly,
       TestExceptionPorts::kOtherThreadCrashes},
      {TestExceptionPorts::kSetOutOfProcess,
       TestExceptionPorts::kSetOnTaskAndThreads,
       TestExceptionPorts::kNobodyCrashes},
      {TestExceptionPorts::kSetOutOfProcess,
       TestExceptionPorts::kSetOnTaskAndThreads,
       TestExceptionPorts::kMainThreadCrashes},
      {TestExceptionPorts::kSetOutOfProcess,
       TestExceptionPorts::kSetOnTaskAndThreads,
       TestExceptionPorts::kOtherThreadCrashes},
  };

  for (size_t index = 0; index < arraysize(kTestcases); ++index) {
    const Testcase& testcase = kTestcases[index];
    SCOPED_TRACE(
        base::StringPrintf("index %zu, set_type %d, set_on %d, who_crashes %d",
                           index,
                           testcase.set_type,
                           testcase.set_on,
                           testcase.who_crashes));

    TestExceptionPorts test_exception_ports(
        testcase.set_type, testcase.set_on, testcase.who_crashes);
    test_exception_ports.Run();
  }
}

TEST(ExceptionPorts, HostExceptionPorts) {
  // ExceptionPorts isn’t expected to work as non-root. Just do a quick test to
  // make sure that TargetTypeName() returns the right string, and that the
  // underlying host_get_exception_ports() function appears to be called by
  // looking for a KERN_INVALID_ARGUMENT return value. Or, on the off chance
  // that the test is being run as root, just look for KERN_SUCCESS.
  // host_set_exception_ports() is not tested, because if the test were running
  // as root and the call succeeded, it would have global effects.

  base::mac::ScopedMachSendRight host(mach_host_self());
  ExceptionPorts explicit_host_ports(ExceptionPorts::kTargetTypeHost, host);
  EXPECT_EQ("host", explicit_host_ports.TargetTypeName());

  std::vector<ExceptionPorts::ExceptionHandler> handlers;
  bool rv = explicit_host_ports.GetExceptionPorts(
      ExcMaskAll() | EXC_MASK_CRASH, &handlers);
  if (geteuid() == 0) {
    EXPECT_TRUE(rv);
  } else {
    EXPECT_FALSE(rv);
  }

  ExceptionPorts implicit_host_ports(ExceptionPorts::kTargetTypeHost,
                                     MACH_PORT_NULL);
  EXPECT_EQ("host", implicit_host_ports.TargetTypeName());

  rv = implicit_host_ports.GetExceptionPorts(
      ExcMaskAll() | EXC_MASK_CRASH, &handlers);
  if (geteuid() == 0) {
    EXPECT_TRUE(rv);
  } else {
    EXPECT_FALSE(rv);
  }
}

}  // namespace
