/*
 * This file is distributed as part of the SkySQL Gateway. It is free
 * software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation,
 * version 2.
 * 
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 * 
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * 
 * Copyright SkySQL Ab 2013
 * 
 */

/**
 * @file gateway.c - The gateway entry point.
 *
 * @verbatim
 * Revision History
 *
 * Date		Who			Description
 * 23-05-2013	Massimiliano Pinto	epoll loop test
 * 12-06-2013	Mark Riddoch		Add the -p option to set the
 * 					listening port
 *					and bind addr is 0.0.0.0
 * 19/06/13	Mark Riddoch		Extract the epoll functionality 
 * 21/06/13	Mark Riddoch		Added initial config support
 * 27/06/13
 * 28/06/13 Vilho Raatikka      Added necessary headers, example functions and
 *                              calls to log manager and to query classifier.
 *                              Put example code behind SS_DEBUG macros.
 *
 * @endverbatim
 */

#include <gw.h>
#include <unistd.h>
#include <service.h>
#include <server.h>
#include <dcb.h>
#include <session.h>
#include <modules.h>
#include <config.h>
#include <poll.h>

#include <stdlib.h>
#include <mysql.h>
#include <monitor.h>

#include <sys/stat.h>
#include <sys/types.h>

# include <skygw_utils.h>
# include <log_manager.h>

/*
 * Server options are passed to the mysql_server_init. Each gateway must have a unique
 * data directory that is passed to the mysql_server_init, therefore the data directory
 * is not fixed here and will be updated elsewhere.
 */
static char* server_options[] = {
    "SkySQL Gateway",
    "--datadir=",
    "--skip-innodb",
    "--default-storage-engine=myisam",
    NULL
};

const int num_elements = (sizeof(server_options) / sizeof(char *)) - 1;

static char* server_groups[] = {
    "embedded",
    "server",
    "server",
    "server",
    NULL
};

/* The data directory we created for this gateway instance */
static char	datadir[1024] = "";

/**
 * exit flag for log flusher.
 */
static bool do_exit = FALSE;

/**
 * Flag to indicate whether libmysqld is successfully initialized.
 */
static bool libmysqld_started = FALSE;

static void log_flush_shutdown(void);
static void log_flush_cb(void* arg);
static void libmysqld_done(void);

/**
 * Handler for SIGHUP signal. Reload the configuration for the
 * gateway.
 */
static void sighup_handler (int i)
{
	skygw_log_write( LOGFILE_MESSAGE, "Refreshing configuration following SIGHUP\n");
	config_reload();
}

static void sigterm_handler (int i) {
extern void shutdown_gateway();

	skygw_log_write( LOGFILE_ERROR, "Signal SIGTERM %i received ...Exiting!\n", i);
	shutdown_gateway();
}

/* wrapper for sigaction */
static void signal_set (int sig, void (*handler)(int)) {
	static struct sigaction sigact;
	static int err;

	memset(&sigact, 0, sizeof(struct sigaction));
	sigact.sa_handler = handler;
	GW_NOINTR_CALL(err = sigaction(sig, &sigact, NULL));
	if (err < 0) {
		skygw_log_write( LOGFILE_ERROR,"sigaction() error %s\n", strerror(errno));
		exit(1);
	}
}

int handle_event_errors(DCB *dcb) {

	fprintf(stderr, "#### Handle error function for [%i] is [%s]\n", dcb->state, gw_dcb_state2string(dcb->state));

	if (dcb->state == DCB_STATE_DISCONNECTED) {
		fprintf(stderr, "#### Handle error function, session is %p\n", dcb->session);
		return 1;
	}

#ifdef GW_EVENT_DEBUG
	if (event != -1) {
		fprintf(stderr, ">>>>>> DCB state %i, Protocol State %i: event %i, %i\n", dcb->state, protocol->state, event & EPOLLERR, event & EPOLLHUP);
		if(event & EPOLLHUP)
			fprintf(stderr, "EPOLLHUP\n");

		if(event & EPOLLERR)
			fprintf(stderr, "EPOLLERR\n");

		if(event & EPOLLPRI)
			fprintf(stderr, "EPOLLPRI\n");
	}
#endif

	if (dcb->state != DCB_STATE_LISTENING) {
		if (poll_remove_dcb(dcb) == -1) {
				fprintf(stderr, "poll_remove_dcb: from events check failed to delete %i, [%i]:[%s]\n", dcb->fd, errno, strerror(errno));
		}

#ifdef GW_EVENT_DEBUG
		fprintf(stderr, "closing fd [%i]=[%i], from events\n", dcb->fd, protocol->fd);
#endif
		if (dcb->fd) {
			//fprintf(stderr, "Client protocol dcb->protocol %p\n", dcb->protocol);

			gw_mysql_close((MySQLProtocol **)&dcb->protocol);
			fprintf(stderr, "Client protocol dcb->protocol %p\n", dcb->protocol);

			dcb->state = DCB_STATE_DISCONNECTED;

		}
	}

	fprintf(stderr, "Return from error handling, dcb is %p\n", dcb);
	//free(dcb->session);
	dcb->state = DCB_STATE_FREED;

	fprintf(stderr, "#### Handle error function RETURN for [%i] is [%s]\n", dcb->state, gw_dcb_state2string(dcb->state));
	//free(dcb);

	return 1;
}

int handle_event_errors_backend(DCB *dcb) {

	fprintf(stderr, "#### Handle Backend error function for %i\n", dcb->fd);

#ifdef GW_EVENT_DEBUG
	if (event != -1) {
		fprintf(stderr, ">>>>>> Backend DCB state %i, Protocol State %i: event %i, %i\n", dcb->state, dcb->proto_state, event & EPOLLERR, event & EPOLLHUP);
		if(event & EPOLLHUP)
			fprintf(stderr, "EPOLLHUP\n");

		if(event & EPOLLERR)
			fprintf(stderr, "EPOLLERR\n");

		if(event & EPOLLPRI)
			fprintf(stderr, "EPOLLPRI\n");
	}
#endif

	if (dcb->state != DCB_STATE_LISTENING) {
		if (poll_remove_dcb(dcb) == -1) {
				fprintf(stderr, "Backend poll_remove_dcb: from events check failed to delete %i, [%i]:[%s]\n", dcb->fd, errno, strerror(errno));
		}

#ifdef GW_EVENT_DEBUG
		fprintf(stderr, "Backend closing fd [%i]=%i, from events check\n", dcb->fd, protocol->fd);
#endif
		if (dcb->fd) {
			dcb->state = DCB_STATE_DISCONNECTED;
			fprintf(stderr, "Freeing backend MySQL conn %p, %p\n", dcb->protocol, &dcb->protocol);
			gw_mysql_close((MySQLProtocol **)&dcb->protocol);
			fprintf(stderr, "Freeing backend MySQL conn %p, %p\n", dcb->protocol, &dcb->protocol);
		}
	}

	return 0;
}

/**
 * Cleanup the temporary data directory we created for the gateway
 */
void
datadir_cleanup()
{
char	buf[1024];

	if (datadir[0] && access(datadir, F_OK) == 0)
	{
		sprintf(buf, "rm -rf %s", datadir);
		system(buf);
	}
}


static libmysqld_done(void)
{
        if (libmysqld_started) {
            mysql_library_end();
        }
}


/**
 * The main entry point into the gateway
 *
 * @param argc	The argument count
 * @param argv	The arguments themselves
 */
int
main(int argc, char **argv)
{
int		daemon_mode = 1;
sigset_t	sigset;
int		i, n, n_threads, n_services;
void		**threads;
char		mysql_home[1024], buf[1024], *home, *cnf_file = NULL;
char		ddopt[1024];
void*           log_flush_thr = NULL;
ssize_t         log_flush_timeout_ms = 0;

        int 	l;

        l = atexit(skygw_logmanager_exit);

        if (l != 0) {
            fprintf(stderr, "Couldn't register exit function.\n");
        }

        atexit(datadir_cleanup);


        for (n = 0; n < argc; n++)
        {
            if (strcmp(argv[n], "-d") == 0)
            {
                /** Debug mode, maxscale runs in this same process */
                daemon_mode = 0;
            }
            if (strncmp(argv[n], "-c", 2) == 0)
            {
                int s=2;
                
                while (argv[n][s] == 0 && s<10) s++;
                
                if (s==10) {
                        skygw_log_write(
                                LOGFILE_ERROR,
                                "Fatal : missing file name. \n"
                                "Unable to find a MaxScale configuration file, "
                                "either install one in /etc/MaxScale.cnf, "
                                "$MAXSCALE_HOME/etc/MaxScale.cnf "
                                "or use the -c option with configuration file "
                                "name. Exiting.\n");
                }
                cnf_file = &argv[n][s];
            }
        }
        
        /**
         * Maxscale must be daemonized before opening files, initializing
         * embedded MariaDB and in general, as early as possible.
         */
        if (daemon_mode == 1)
        {
            if (sigfillset(&sigset) != 0) {
                skygw_log_write(
                                LOGFILE_ERROR,
                                "sigfillset() error %s\n",
                                strerror(errno));
                return 1;
            }
            
            if (sigdelset(&sigset, SIGHUP) != 0) {
                skygw_log_write(
                                LOGFILE_ERROR,
                                "sigdelset(SIGHUP) error %s\n",
                                strerror(errno));
            }
            
            if (sigdelset(&sigset, SIGTERM) != 0) {
                skygw_log_write(
                                LOGFILE_ERROR,
                                "sigdelset(SIGTERM) error %s\n",
                                strerror(errno));
            }
            
            if (sigprocmask(SIG_SETMASK, &sigset, NULL) != 0) {
                skygw_log_write(
                                LOGFILE_ERROR,
                                "sigprocmask() error %s\n",
                                strerror(errno));
            }
            
            signal_set(SIGHUP, sighup_handler);
            signal_set(SIGTERM, sigterm_handler);
            
            gw_daemonize();
        }

        l = atexit(libmysqld_done);

        if (l != 0) {
            fprintf(stderr, "Couldn't register exit function.\n");
        }
        
        if ((home = getenv("MAXSCALE_HOME")) != NULL)
        {
            sprintf(mysql_home, "%s/mysql", home);
            setenv("MYSQL_HOME", mysql_home, 1);
            sprintf(buf, "%s/etc/MaxScale.cnf", home);
            if (access(buf, R_OK) == 0)
                cnf_file = buf;
        }
        if (cnf_file == NULL && access("/etc/MaxScale.cnf", R_OK) == 0)
            cnf_file = "/etc/MaxScale.cnf";

        /*
         * Set a data directory for the mysqld library, we use
         * a unique directory name to avoid clauses if multiple
         * instances of the gateway are beign run on the same
         * machine.
         */
        if (home)
        {
            sprintf(datadir, "%s/data%d", home, getpid());
            mkdir(datadir, 0777);
        }
        else
        {
            sprintf(datadir, "/tmp/MaxScale/data%d", getpid());
            mkdir("/tmp/MaxScale", 0777);
            mkdir(datadir, 0777);
        }
        
	/*
	 * If $MAXSCALE_HOME is set then write the logs into $MAXSCALE_HOME/log.
	 * The skygw_logmanager_init expects to take arguments as passed to main
	 * and proesses them with getopt, therefore we need to give it a dummy
	 * argv[0]
	 */
	if (home)
	{
		char 	buf[1024];
		char	*argv[4];

		sprintf(buf, "%s/log", home);
		mkdir(buf, 0777);
		argv[0] = "MaxScale";
		argv[1] = "-g";
		argv[2] = buf;
		argv[3] = NULL;
		skygw_logmanager_init(3, argv);
	}

	if (cnf_file == NULL) {
		skygw_log_write_flush(
			LOGFILE_ERROR,
			"Fatal : Unable to find a MaxScale configuration "
                        "file, either install one in /etc/MaxScale.cnf, "
                        "$MAXSCALE_HOME/etc/MaxScale.cnf "
			"or use the -c option. Exiting.\n");
		exit(1);
	}
    
	/* Update the server options */
	for (i = 0; server_options[i]; i++)
	{
		if (!strcmp(server_options[i], "--datadir="))
		{
			sprintf(ddopt, "--datadir=%s", datadir);
			server_options[i] = ddopt;
		}
	}
    
	if (mysql_library_init(num_elements, server_options, server_groups))
	{
		skygw_log_write_flush(
                LOGFILE_ERROR,
                "Fatal : mysql_library_init failed, %s. This is mandatory "
                "component, required by router services and the MaxScale core, "
                "the MaxScale can't continue without it. Exiting.\n"
                "%s : %d",
                mysql_error(NULL),
                __FILE__,
                __LINE__);
		exit(1);
	}
        libmysqld_started = TRUE;
            
	if (!config_load(cnf_file))
	{
		skygw_log_write_flush(
                LOGFILE_ERROR,
                "Failed to load MaxScale configuration file %s", cnf_file);
		exit(1);
	}
    
	skygw_log_write(
                    LOGFILE_MESSAGE,
                    "SkySQL MaxScale (C) SkySQL Ab 2013"); 
	skygw_log_write(
                    LOGFILE_MESSAGE,
                    "MaxScale is starting, PID %i",
                    getpid());
    
	poll_init();
    
	/*
	 * Start the services that were created above
	 */
	n_services = serviceStartAll();
	skygw_log_write(LOGFILE_MESSAGE, "Started modules succesfully.");

        /**
         * Start periodic log flusher thread.
         */
        log_flush_timeout_ms = 1000;
        log_flush_thr = thread_start(log_flush_cb, (void *)&log_flush_timeout_ms);
	/*
	 * Start the polling threads, note this is one less than is
	 * configured as the main thread will also poll.
	 */
	n_threads = config_threadcount();
	threads = (void **)calloc(n_threads, sizeof(void *));
	for (n = 0; n < n_threads - 1; n++)
		threads[n] = thread_start(poll_waitevents, (void *)(n + 1));
	poll_waitevents((void *)0);
	for (n = 0; n < n_threads - 1; n++)
		thread_wait(threads[n]);

        free(threads);
        
        /**
         * Wait the flush thread.
         */
        thread_wait(log_flush_thr);

	/* Stop all the monitors */
	monitorStopAll();
        
	skygw_log_write(
                    LOGFILE_MESSAGE,
                    "MaxScale shutdown, PID %i\n",
                    getpid());

	return 0;
} // End of main

/**
 * Shutdown the gateway
 */
void
shutdown_gateway()
{
	poll_shutdown();
        log_flush_shutdown();
}

static void log_flush_shutdown(void)
{
        do_exit = TRUE;
}

static void log_flush_cb(
        void* arg)
{
        ssize_t timeout_ms = *(ssize_t *)arg;

        skygw_log_write(LOGFILE_MESSAGE, "Started MaxScale log flusher.");
        while (!do_exit) {
            skygw_log_flush(LOGFILE_ERROR);
            skygw_log_flush(LOGFILE_MESSAGE);
            skygw_log_flush(LOGFILE_TRACE);
            usleep(timeout_ms*1000);
        }
        skygw_log_write(LOGFILE_MESSAGE, "Finished MaxScale log flusher.");        
}