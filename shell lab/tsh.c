/* 
 * tsh - A tiny shell program with job control
 * 
 * author: asciibase64
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE];  /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */
/* End global variables */


/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* My helper functions */
int Sigprocmask(int how, sigset_t *set, sigset_t *old_set);
int Fork();
int Execve(const char *filename, const char *argv[], const char *envp[]);
int Sigemptyset(sigset_t *set);
int Sigfillset(sigset_t *set);
int Sigaddset(sigset_t *set, int signum);
int Kill(pid_t pid, int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs); 
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid); 
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine 
 */
int main(int argc, char **argv) 
{
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h':             /* print help message */
            usage();
	    break;
        case 'v':             /* emit additional diagnostic info */
            verbose = 1;
	    break;
        case 'p':             /* don't print a prompt */
            emit_prompt = 0;  /* handy for automatic testing */
	    break;
	default:
            usage();
	}
    }

    /* Install the signal handlers */

    /* These are the ones you will need to implement */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler); 

    /* Initialize the job list */
    initjobs(jobs);

    /* Execute the shell's read/eval loop */
    while (1) {

        /* Read command line */
        if (emit_prompt) {
            printf("%s", prompt);
            fflush(stdout);
        }
        if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
            app_error("fgets error");
        if (feof(stdin)) { /* End of file (ctrl-d) */
            fflush(stdout);
            exit(0);
        }

        /* Evaluate the command line */
        eval(cmdline);
        fflush(stdout);
        fflush(stdout);
    } 

    exit(0); /* control never reaches here */
}
  
/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
*/
void eval(char *cmdline) 
{
    char *argv[MAXARGS];
    int bg = parseline(cmdline, argv);  /* background job? */
    int builtin = builtin_cmd(argv);    /* is built-in command? */

    sigset_t mask_one, mask_all, prev;

    Sigemptyset(&mask_one);
    Sigaddset(&mask_one, SIGCHLD);
    Sigfillset(&mask_all);

    if (!builtin) {
        Sigprocmask(SIG_BLOCK, &mask_one, &prev);   /* block SIGCHLD */
        pid_t pid;

        if ( (pid = Fork()) == 0 ) {                /* child process */
            Sigprocmask(SIG_SETMASK, &prev, NULL);  /* unblock SIGCHLD */
            setpgid(0, 0);
            Execve(argv[0], argv, environ);
        }

        Sigprocmask(SIG_BLOCK, &mask_all, NULL);   /* block all signals for atomic operation */
        if (bg) {
            addjob(jobs, pid, BG, cmdline);
            struct job_t *j = getjobpid(jobs, pid);
            printf("[%d] (%d) %s", j->jid, j->pid, j->cmdline);
        }
        else {
            addjob(jobs, pid, FG, cmdline);
            waitfg(pid);
        }            
        Sigprocmask(SIG_SETMASK, &prev, NULL);      /* unblock SIGCHLD */
    }
    return;
}

/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.  
 */
int parseline(const char *cmdline, char **argv) 
{
    static char array[MAXLINE]; /* holds local copy of command line */
    char *buf = array;          /* ptr that traverses command line */
    char *delim;                /* points to first space delimiter */
    int argc;                   /* number of args */
    int bg;                     /* background job? */

    strcpy(buf, cmdline);
    buf[strlen(buf)-1] = ' ';       /* replace trailing '\n' with space */
    while (*buf && (*buf == ' '))   /* ignore leading spaces */
	    buf++;

    /* Build the argv list */
    argc = 0;
    if (*buf == '\'') {
	    buf++;
	    delim = strchr(buf, '\'');
    }
    else {
	    delim = strchr(buf, ' ');
    }

    while (delim) {
	    argv[argc++] = buf;
	    *delim = '\0';                /* truncate the arg. smart solution */
	    buf = delim + 1;
	    while (*buf && (*buf == ' ')) /* ignore spaces */
	        buf++;

	    if (*buf == '\'') {
	        buf++;
	        delim = strchr(buf, '\'');
	    }
	    else {
	        delim = strchr(buf, ' ');
	    }
    }
    argv[argc] = NULL;
    
    if (argc == 0)  /* ignore blank line */
	    return 1;

    /* should the job run in the background? */
    if ((bg = (*argv[argc-1] == '&')) != 0) {
	    argv[--argc] = NULL;
    }
    return bg;
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.  
 */
int builtin_cmd(char **argv) 
{
    if (argv[0] == NULL)
        return 1;

    int builtin = 0;

    char *cmdlist[4] = {"quit", "jobs", "bg", "fg"};
    int idx = -1;

    int i;
    for (i = 0; i < 4; i++) {
        if (strcmp(argv[0], cmdlist[i]) == 0) {
            idx = i;
            builtin = 1;
        }
    }        

    switch (idx) {
    case 0:
        exit(0);
    case 1:
        listjobs(jobs);
        break;
    case 2:
        do_bgfg(argv);
        break;
    case 3:
        do_bgfg(argv);
        break;
    default:
        break;
    }

    return builtin;
}

/* 
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv) 
{
    if (argv[1] == NULL) {
        printf("%s command requires PID or %%jobid argument\n", argv[0]);
        return;
    }

    if (isalpha(*argv[1])) {
        printf("%s: argument must be a PID or %%jobid\n", argv[0]);
        return;
    }

    /* get the job who needs to continue */
    struct job_t *j;
    char *delim = strchr(argv[1], '%');
    if (delim == NULL) {            /* use PID */
        pid_t pid = atoi(argv[1]);
        j = getjobpid(jobs, pid);

        if (j == NULL) {
            printf("(%d): No such process\n", pid);
            return;
        }
    }
    else {                          /* use JID */
        int jid = atoi(++argv[1]);
        j = getjobjid(jobs, jid);

        if (j == NULL) {
            printf("%%%s: No such job\n", argv[1]);
            return;
        }
    }

    if (strcmp(argv[0], "bg") == 0) {                /* continue in background */
        j->state = BG;
        printf("[%d] (%d) %s", j->jid, j->pid, j->cmdline);
        Kill(-j->pid, SIGCONT);
    }
    else {                                      /* continue in foreground */
        j->state = FG;
        Kill(-j->pid, SIGCONT);
        waitfg(j->pid);
    }
    return;
}

/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid)
{
    sigset_t mask;
    Sigemptyset(&mask);
    
    while (fgpid(jobs) == pid)
        sigsuspend(&mask);

    return;
}

/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.  
 */
void sigchld_handler(int sig) 
{
    int olderrno = errno;
    
    sigset_t mask, prev;
    Sigfillset(&mask);

    pid_t pid;
    int status;
    
    while ( (pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        Sigprocmask(SIG_BLOCK, &mask, &prev);   /* block all */
        if (WIFEXITED(status))      /* job exit */
            deletejob(jobs, pid);
        if (WIFSIGNALED(status)) {  /* job interrupt */
            printf("Job [%d] (%d) terminated by signal %d\n", pid2jid(pid), pid, WTERMSIG(status));
            deletejob(jobs, pid);
        }
        if (WIFSTOPPED(status)) {   /* job suspend */
            struct job_t *j = getjobpid(jobs, pid);
            if (j) {
                j->state = ST;
                printf("Job [%d] (%d) stopped by signal %d\n", j->jid, pid, WSTOPSIG(status));
            }
            else 
                printf("cannot find a job that corresponding to pid: %d", pid);
        }    
        Sigprocmask(SIG_SETMASK, &prev, NULL);  /* unblock all */
    }

    /* waitpid sets errno to ECHILD if there's no more zombie process */
    /* be careful: we have to ignore EINTR too */
    if (errno != ECHILD && errno != EINTR)
        unix_error("waitpid error");

    errno = olderrno;
    return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig) 
{
    int olderrno = errno;

    pid_t pid = fgpid(jobs);

    /* we don't need to response when there's no foreground job */
    if (pid == 0) {
        errno = olderrno;
        return;
    }
    
    sigset_t mask, prev;
    Sigfillset(&mask);
    
    Sigprocmask(SIG_BLOCK, &mask, &prev);   /* block all signals */

    /* send signal to all the process in the same group to pid */
    Kill(-pid, sig);

    Sigprocmask(SIG_SETMASK, &prev, NULL);  /* unblock all signals */

    errno = olderrno;
    return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig) 
{
    int olderrno = errno;

    pid_t pid = fgpid(jobs);

    if (pid == 0) {
        errno = olderrno;
        return;
    }
    
    sigset_t mask, prev;
    Sigfillset(&mask);
    
    Sigprocmask(SIG_BLOCK, &mask, &prev);   /* block all signals */
    
    Kill(-pid, sig);
    
    Sigprocmask(SIG_SETMASK, &prev, NULL);  /* unblock all signals */

    errno = olderrno;
    return;
}

/*********************
 * End signal handlers
 *********************/

/*********************
 * My helper funcitons
 *********************/

 /* Sigprogmask - Wrapper for sigprocmask funciton */
int Sigprocmask(int how, sigset_t *set, sigset_t *oldset)
{
    int success;
    if ( (success = sigprocmask(how, set, oldset)) < 0 )
        unix_error("sigprocmask error");
    return success;
}

/* Fork - Wrapper for sigprocmask function */
int Fork()
{
    pid_t pid;
    if ( (pid = fork()) < 0 )
        unix_error("fork error");
    return pid;
}

/* Execve - Wrapper for execve function */
int Execve(const char *filename, const char **argv, const char *envp[])
{
    int success;
    if ( (success = execve(filename, argv, envp)) < 0) {
        printf("%s: Command not found\n", filename);
        exit(0);
    }

    return success;
}

/* Sigemptyset - Wrapper for sigemptyset function */
int Sigemptyset(sigset_t *set)
{
    int success;
    if ( (success = sigemptyset(set)) < 0)
        unix_error("sigemptyset error");
    return success;
}

/* Sigfillset - Wrapper for sigfillset function */
int Sigfillset(sigset_t *set)
{
    int success;
    if ( (success = sigfillset(set)) < 0)
        unix_error("sigfillset error");
    return success;
}

/* Sigaddset - Wrapper for sigaddset function */
int Sigaddset(sigset_t *set, int signum)
{
    int success;
    if ( (success = sigaddset(set, signum)) < 0)
        unix_error("sigaddset error");
    return success;
}

/* Kill - Wrapper for kill function */
int Kill(pid_t pid, int sig)
{
    int success;
    if ( (success = kill(pid, sig)) < 0 && errno != ESRCH)
        unix_error("kill error");
    return success;
}

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	    clearjob(&jobs[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs) 
{
    int i, max=0;

    for (i = 0; i < MAXJOBS; i++)
	    if (jobs[i].jid > max)
	        max = jobs[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) 
{
    int i;
    
    if (pid < 1)
	    return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == 0) {
            jobs[i].pid = pid;
            jobs[i].state = state;
            jobs[i].jid = nextjid++;
            if (nextjid > MAXJOBS)
                nextjid = 1;
            strcpy(jobs[i].cmdline, cmdline);
            if(verbose){
                printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
            return 1;
        }
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) 
{
    int i;

    if (pid < 1)
	    return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == pid) {
            clearjob(&jobs[i]);
            nextjid = maxjid(jobs)+1;
            return 1;
        }
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].state == FG)
            return jobs[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
	    return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].pid == pid)
            return &jobs[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
    int i;

    if (jid < 1)
	    return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].jid == jid)
            return &jobs[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) 
{
    int i;

    if (pid < 1)
	    return 0;
    for (i = 0; i < MAXJOBS; i++)
	    if (jobs[i].pid == pid) {
            return jobs[i].jid;
        }
    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs) 
{
    int i;
    
    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid != 0) {
            printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
            switch (jobs[i].state) {
            case BG: 
                printf("Running ");
                break;
            case FG: 
                printf("Foreground ");
                break;
            case ST: 
                printf("Stopped ");
                break;
            default:
                printf("listjobs: Internal error: job[%d].state=%d ", 
                i, jobs[i].state);
            }
            printf("%s", jobs[i].cmdline);
        }
    }
}

/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void) 
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) 
{
    struct sigaction action, old_action;

    action.sa_handler = handler;  
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
	unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig) 
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}