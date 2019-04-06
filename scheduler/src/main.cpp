#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cassert>
#include <cinttypes>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <linux/limits.h>
#include "perf.hpp"
#include "time.hpp"

#ifndef SCHEDULER_TYPE
#   error Please define SCHEDULER_TYPE
#endif

#if !(SCHEDULER_TYPE >= 0 && SCHEDULER_TYPE <= 2)
#   error SCHEDULER_TYPE must be 0, 1 or 2
#endif

#define SCHEDULER_TYPE_COLLECT 0
#define SCHEDULER_TYPE_PREDICTOR 1
#define SCHEDULER_TYPE_AGENT 2

char** environ;

static FILE* collect_stream = 0;
static int scheduler_input_pipe = -1;
static int scheduler_output_pipe = -1;
static int scheduler_pid = -1;
static int application_pid = -1;
static uint64_t application_start_time = 0;

enum State
{
    STATE_L,
    STATE_B,
    STATE_BL,
};

static State current_state = STATE_BL;
static double current_state_mips = 0.0;

static double get_cpu_usage()
{
    char buffer[256];
    sprintf(buffer, "ps -p %d -mo pcpu", ::application_pid);
    FILE* stream = popen(buffer, "r");
    if(!stream)
    {
        perror("failed to collect cpu usage");
        return 0.0;
    }
    
    double total = 0.0;
    fgets(buffer, sizeof(buffer), stream); // skip %CPU
    if(fscanf(stream, "%lf", &total) != 1)
        total = 0.0;

    fclose(stream);

    return total;
}

static void send_to_scheduler(const char* fmt, ...)
{
    char buffer[512];

    va_list va;
    va_start(va, fmt);
    int count = vsnprintf(buffer, sizeof(buffer) - 1, fmt, va);
    va_end(va);

    if(count < 0)
    {
        perror("scheduler: failed to vsnprintf during send_to_scheduler");
        abort();
    }
    else
    {
        buffer[count++] = '\n';
        buffer[count] = '\0';

        int written = write(scheduler_input_pipe, buffer, count);
        if(written == -1)
        {
            perror("scheduler: failed to write to scheduler");
            abort();
        }
        else if(written != count)
        {
            fprintf(stderr, "scheduler: count mismatch during send_to_scheduler\n");
            abort();
        }
    }
}

static void recv_from_scheduler(const char* fmt, ...)
{
    int count = 0;
    char buffer[512];

    while(count == 0 || buffer[count-1] != '\n')
    {
        const auto result = read(scheduler_output_pipe, buffer, sizeof(buffer) - count);
        if(result <= 0)
        {
            perror("scheduler: failed to read from scheduler pipe\n");
            abort();
        }

        count += result;
        assert(count < (int) sizeof(buffer) - 1);
    }

    va_list va;
    va_start(va, fmt);
    vsscanf(buffer, fmt, va);
    va_end(va);
}

static void cleanup()
{
    fprintf(stderr, "scheduler: cleaning up\n");

    if(application_pid != -1)
    {
        kill(application_pid, SIGTERM);
        waitpid(application_pid, nullptr, 0);
        application_pid = -1;
    }

    if(scheduler_pid != -1)
    {
        kill(scheduler_pid, SIGTERM);
        waitpid(scheduler_pid, nullptr, 0);
        scheduler_pid = -1;
    }

    if(scheduler_input_pipe != -1)
    {
        close(scheduler_input_pipe);
        scheduler_input_pipe = -1;
    }

    if(scheduler_output_pipe != -1)
    {
        close(scheduler_output_pipe);
        scheduler_output_pipe = -1;
    }

    if(collect_stream != 0)
    {
        fclose(collect_stream);
        collect_stream = 0;
    }
}

static bool create_logging_file()
{
    char filename[PATH_MAX];
    sprintf(filename, "scheduler_%d.csv", getpid());
    collect_stream = fopen(filename, "w");
    if(!collect_stream)
    {
        perror("scheduler: failed to open logging file");
        return false;
    }
    fprintf(stderr, "scheduler: collecting to file %s\n", filename);
    return true;
}

static bool create_time_file(uint64_t time_ms)
{
    char filename[PATH_MAX];
    sprintf(filename, "scheduler_%d.time", getpid());
    FILE* time_stream = fopen(filename, "w");
    if(!time_stream)
    {
        perror("scheduler: failed to open time file");
        return false;
    }
    fprintf(time_stream, "%" PRIu64, time_ms);
    return true;
}

static bool spawn_scheduling_process(const char* command)
{
    int inpipefd[2] = {-1, -1};
    int outpipefd[2] = {-1, -1};

    if(pipe(inpipefd) == -1 || pipe(outpipefd) == -1)
    {
        perror("scheduler: failed to create scheduling pipes.");
        return false;
    }

    int pid = fork();
    if(pid == -1)
    {
        perror("scheduler: failed to fork scheduler");
        close(inpipefd[0]);
        close(inpipefd[1]);
        close(outpipefd[0]);
        close(outpipefd[1]);
        return false;
    }
    else if(pid == 0)
    {
        dup2(outpipefd[0], STDIN_FILENO);
        dup2(inpipefd[1], STDOUT_FILENO);

        close(outpipefd[1]);
        close(inpipefd[0]);

        // receive SIGTERM once the parent process dies
        prctl(PR_SET_PDEATHSIG, SIGTERM);

        execl("/bin/sh", "sh", "-c", command, NULL);
        perror("scheduler: execl failed");
        return false;
    }
    else
    {
        close(outpipefd[0]);
        close(inpipefd[1]);
        ::scheduler_pid = pid;
        ::scheduler_input_pipe = outpipefd[1];
        ::scheduler_output_pipe = inpipefd[0];
        return true;
    }
}

static bool spawn_scheduled_application(char* argv[])
{
    int pid = fork();
    if(pid == -1)
    {
        perror("scheduler: failed to fork scheduled application");
        return false;
    }
    else if(pid == 0)
    {
        execvp(argv[0], argv);
        perror("scheduler: execvp failed");
        return false;
    }
    else
    {
        ::application_pid = pid;
        ::application_start_time = get_time();
        return true;
    }
}

static void update_scheduler()
{
    const double cpu_usage = get_cpu_usage();

    uint64_t total_cycles = 0;
    uint64_t total_instructions = 0;
    uint64_t total_cache_miss = 0;
    uint64_t total_branch_inst = 0;
    uint64_t total_branch_miss = 0;

    for(int cpu = 0, max_cpu = perf_nprocs(); cpu < max_cpu; ++cpu)
    {
        const auto hw_data = perf_consume_hw(cpu);
        total_cycles += hw_data.cpu_cycles;
        total_instructions += hw_data.instructions;
        total_cache_miss += hw_data.cache_misses;
        total_branch_inst += hw_data.branch_instructions;
        total_branch_miss += hw_data.branch_misses;
    }

    const uint64_t elapsed_time = to_millis(get_time() - ::application_start_time);
    const double mkpi = ((double)(total_cache_miss) / (double)(total_instructions)) * 1000.0;
    const double bmiss = double(total_branch_miss) / double(total_branch_inst);
    const double ipc = double(total_instructions) / double(total_cycles);

    State next_state = current_state;
    double next_state_mips = 0.0;

#if SCHEDULER_TYPE == SCHEDULER_TYPE_COLLECT

    fprintf(collect_stream, "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
            elapsed_time, total_cycles, total_instructions, total_cache_miss, total_branch_inst, total_branch_miss);

#elif SCHEDULER_TYPE == SCHEDULER_TYPE_PREDICTOR

    for(int i = 0; i <= 2; ++i)
    {
        assert(i == STATE_BL || i == STATE_B || i == STATE_L);

        const bool has_big = (i == STATE_BL || i == STATE_B);
        const bool has_little = (i == STATE_BL || i == STATE_L);

        double expected_mips;
        send_to_scheduler("%a %a %a %d %d %a", mkpi, bmiss, ipc, has_big, has_little, cpu_usage);
        recv_from_scheduler("%lf", &expected_mips);

        //fprintf(stderr, "%lf %lf %lf %d %d \n", mkpi, bmiss, ipc, has_big, has_little);
        //fprintf(stderr, "Mips predictor %lf\n", expected_mips);

        if(expected_mips > next_state_mips)
        {
            next_state_mips = expected_mips;
            next_state = static_cast<State>(i);
        }
    }

#elif SCHEDULER_TYPE == SCHEDULER_TYPE_AGENT

    char agent_reply[64];
    send_to_scheduler("%a %a %a", mkpi, bmiss, ipc);
    recv_from_scheduler("%s", agent_reply);
    if(!strcmp(agent_reply, "4L"))
        next_state = STATE_L;
    else if(!strcmp(agent_reply, "4B"))
        next_state = STATE_B;
    else if(!strcmp(agent_reply, "4B4L"))
        next_state = STATE_BL;
    else
        fprintf(stderr, "scheduler: scheduling agent replied with an invalid state: %s\n", agent_reply);

#endif

    if(::application_pid != -1 && next_state != current_state)
    {
        char buffer[512];
        auto cfg = (next_state == STATE_L? "0-3" :
                    next_state == STATE_B? "4-7" :
                                           "0-7");
        sprintf(buffer, "taskset -pac %s %d >/dev/null", cfg, application_pid);
        //fprintf(stderr, "scheduler: %s\n", buffer);

        int status = system(buffer);
        if(status == -1)
        {
            perror("scheduler: system() failed");
        }
        else if(status != 0)
        {
            fprintf(stderr, "scheduler: taskset returned %d :(\n", status);
        }

        current_state = next_state;
        current_state_mips = next_state_mips;
    }
}

int main(int argc, char* argv[])
{
    if(argc < 2)
    {
        fprintf(stderr, "usage: %s command args...\n", argv[0]);
        return 1;
    }

#if SCHEDULER_TYPE == SCHEDULER_TYPE_COLLECT
    if(!create_logging_file())
    {
        cleanup();
        return 1;
    }
#elif SCHEDULER_TYPE == SCHEDULER_TYPE_PREDICTOR
    if(!spawn_scheduling_process("python3 ./predictor.py"))
    {
        cleanup();
        return 1;
    }
#elif SCHEDULER_TYPE == SCHEDULER_TYPE_AGENT
    if(!spawn_scheduling_process("python3 ./agent.py"))
    {
        cleanup();
        return 1;
    }
#endif

    if(!spawn_scheduled_application(&argv[1]))
    {
        cleanup();
        return 1;
    }

    perf_init();

    while(::application_pid != -1)
    {
        usleep(20000); // 20ms

        int pid = waitpid(::application_pid, NULL, WNOHANG);
        if(pid == -1)
        {
            perror("scheduler: waitpid in main loop failed");
        }
        else if(pid != 0)
        {
            assert(pid == ::application_pid);
            application_pid = -1;
            update_scheduler(); // last tick
        }
        else
        {
            update_scheduler();
        }
    }

    create_time_file(to_millis(get_time() - ::application_start_time));

    fprintf(stderr, "scheduler: main application finished\n");
    perf_shutdown();
    cleanup();

    return 0;
}
