/*****************************************************
 * Copyright Grégory Mounié 2008-2015                *
 *           Simon Nieuviarts 2002-2009              *
 * This code is distributed under the GLPv3 licence. *
 * Ce code est distribué sous la licence GPLv3+.     *
 *****************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "variante.h"
#include "readcmd.h"

#include <sys/types.h>
#include <sys/wait.h>

#include <fcntl.h>
#include <signal.h>


#include <unistd.h>


#ifndef VARIANTE
#error "Variante non défini !!"
#endif

/* Guile (1.8 and 2.0) is auto-detected by cmake */
/* To disable Scheme interpreter (Guile support), comment the
 * following lines.  You may also have to comment related pkg-config
 * lines in CMakeLists.txt.
 */

#if USE_GUILE == 1
#include <libguile.h>


struct Process {
    pid_t pid;
    char* command;
    struct Process* next;
};

struct listProcess {
    struct Process* head;
};

void add_process(struct listProcess* list, pid_t pid, char** command) {
    struct Process* new_process = (struct Process*)malloc(sizeof(struct Process));
    new_process->pid = pid;
	new_process->command = malloc(10*sizeof(char));
	strcpy(new_process->command, command[0]);
    new_process->next = NULL;

    if (list->head == NULL) {
        list->head = new_process;
    } else {
        struct Process* current = list->head;
        while (current->next != NULL) {
            current = current->next;
        }
        current->next = new_process;
    }
}

struct Process* find(struct listProcess* list, pid_t pid) {
	struct Process* current = list->head;
	// struct Process* previous = NULL;

	while (current != NULL && current->pid != pid) {
		current = current->next;
	}

	if (current)
		return current;
	return NULL;
}


void remove_process(struct listProcess* list, pid_t pid) {
    struct Process* current = list->head;
    struct Process* previous = NULL;

    while (current != NULL) {
        if (current->pid == pid) {
            if (previous == NULL) {
                list->head = current->next;
            } else {
                previous->next = current->next;
            }
			free(current->command);
            free(current);
            break;
        }
        previous = current;
        current = current->next;
    }
}

void display_bg_ps(struct listProcess* list) {
    struct Process* current = list->head;

    while (current != NULL) {
        int status;
        pid_t result = waitpid(current->pid, &status, WNOHANG);
        if (result == -1) {
            fprintf(stderr, "Error waiting for process %d\n", current->pid);
        } else if (result == 0) {
            printf("%d %s\n", current->pid, current->command);
        } else {
            remove_process(list, current->pid);
        }
        current = current->next;
    }
}


void redirect(char* input_stream, char* output_stream) {
	if (input_stream != NULL) {
		int fd = open(input_stream, O_RDONLY);
		dup2(fd, 0);
		close(fd);
	}
	if (output_stream != NULL) {
		int fd = open(output_stream, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		dup2(fd, 1);
		close(fd);
	}
}

void execute_pipe(struct cmdline *line) {
	pid_t pid;
	// int status;
	// char **arg1[] = line->seq[0];
	// char **arg2[] = line->seq[1];
	int fds[2];

	pipe(fds);

	// first process
	pid = fork();
	if(pid == 0) { 
		// connect stdout to the write end of the pipe
		dup2(fds[1], 1); 
		close(fds[1]);
		close(fds[0]);
		redirect(line->in, NULL);
		execvp(line->seq[0][0], line->seq[0]);
	}

	// second process
	pid = fork();
	if(pid == 0) {
		// connect stdin to the read end of the pipe
		dup2(fds[0], 0);
		close(fds[0]);
		close(fds[1]);
		redirect(NULL, line->out);
		execvp(line->seq[1][0],line->seq[1]);
	}

	// main process
	close(fds[0]);
	close(fds[1]);
	waitpid(pid,NULL,0);
	waitpid(pid,NULL,0);
}


void execute_multiple_pipes(struct cmdline *line, struct listProcess* bg_ps) {
	// count number of pipes
	int nb_pipes = 0;
	while (line->seq[nb_pipes]) {nb_pipes += 1;}
	nb_pipes -= 1;	

	pid_t pid;
	int fds[2*nb_pipes];

	// open pipes
	for (int i = 0; i < nb_pipes; i++) {
		pipe(fds + i*2);
	}

	// first process
	pid = fork();
	if(pid == 0) { 
		// connect stdout to the write end of the pipe
		dup2(fds[1], 1); 
		redirect(line->in, NULL);
		for (int j = 0; j < 2*nb_pipes; j++) {close(fds[j]);}
		execvp(line->seq[0][0], line->seq[0]);
	}

	// intermediate processes
	for (int i = 1; i < nb_pipes; i++) {  // nb_pipes + 1 == nb_commands
		pid = fork();
		if (pid == 0) {
			dup2(fds[2*i+1], 1);
			redirect(line->in, NULL);
			dup2(fds[2*i - 2], 0);
			redirect(NULL, line->out);
			for (int j = 0; j < 2*nb_pipes; j++) {close(fds[j]);}
			execvp(line->seq[i][0], line->seq[i]);
		}
	}

	// last process
	pid = fork();
	if(pid == 0) {
		// connect stdin to the read end of the pipe
		dup2(fds[2*nb_pipes-2], 0);
		redirect(NULL, line->out);
		for (int j = 0; j < 2*nb_pipes; j++) {close(fds[j]);}
		execvp(line->seq[nb_pipes][0],line->seq[nb_pipes]);
	}

	for (int j = 0; j < 2*nb_pipes; j++) {close(fds[j]);}

	for (int i = 0; i < nb_pipes + 1; i++) {
		if (!line->bg) {waitpid(pid, NULL, 0);}
		else if (find(bg_ps, pid) == NULL){
			add_process(bg_ps, pid, line->seq[nb_pipes]);
		}
	}
}


void execute(struct cmdline *line, struct listProcess* bg_process_list) {
	if (strcmp((line->seq[0])[0], "jobs") == 0) {
		display_bg_ps(bg_process_list);
		return;
	}

	pid_t pid = fork();
	switch (pid) {
		case -1:
			perror("fork");
			exit(1);
		case 0: // child
			redirect(line->in, line->out);
			execvp((line->seq[0])[0], line->seq[0]);
		default: // main
			if (line->bg == 0) {waitpid(pid, NULL, 0);}
			else {add_process(bg_process_list, pid, line->seq[0]);}
	}
}

int question6_executer(char *line)
{
	/* Question 6: Insert your code to execute the command line
	 * identically to the standard execution scheme:
	 * parsecmd, then fork+execvp, for a single command.
	 * pipe and i/o redirection are not required.
	 */
	printf("Not implemented yet: can not execute %s\n", line);

	/* Remove this line when using parsecmd as it will free it */
	free(line);
	
	return 0;
}

SCM executer_wrapper(SCM x)
{
        return scm_from_int(question6_executer(scm_to_locale_stringn(x, 0)));
}
#endif


void terminate(char *line) {
#if USE_GNU_READLINE == 1
	/* rl_clear_history() does not exist yet in centOS 6 */
	clear_history();
#endif
	if (line)
	  free(line);
	printf("exit\n");
	exit(0);
}


int main() {
        printf("Variante %d: %s\n", VARIANTE, VARIANTE_STRING);

#if USE_GUILE == 1
        scm_init_guile();
        /* register "executer" function in scheme */
        scm_c_define_gsubr("executer", 1, 0, 0, executer_wrapper);
#endif

	struct listProcess* bg_process_list = malloc(sizeof(struct listProcess));
	bg_process_list->head = NULL;
	while (1) {
		struct cmdline *l;
		char *line=0;
		int i, j;
		char *prompt = "ensishell>";

		/* Readline use some internal memory structure that
		   can not be cleaned at the end of the program. Thus
		   one memory leak per command seems unavoidable yet */
		line = readline(prompt);
		if (line == 0 || ! strncmp(line,"exit", 4)) {
			terminate(line);
		}

#if USE_GNU_READLINE == 1
		add_history(line);
#endif


#if USE_GUILE == 1
		/* The line is a scheme command */
		if (line[0] == '(') {
			char catchligne[strlen(line) + 256];
			sprintf(catchligne, "(catch #t (lambda () %s) (lambda (key . parameters) (display \"mauvaise expression/bug en scheme\n\")))", line);
			scm_eval_string(scm_from_locale_string(catchligne));
			free(line);
                        continue;
                }
#endif

		/* parsecmd free line and set it up to 0 */
		l = parsecmd( & line);

		/* If input stream closed, normal termination */
		if (!l) {
			
			terminate(0);
		}
		

		
		if (l->err) {
			/* Syntax error, read another command */
			printf("error: %s\n", l->err);
			continue;
		}

		if (l->in) printf("in: %s\n", l->in);
		if (l->out) printf("out: %s\n", l->out);
		if (l->bg) printf("background (&)\n");

		/* Display each command of the pipe */
		for (i=0; l->seq[i]!=0; i++) {
			char **cmd = l->seq[i];
			printf("seq[%d]: ", i);
                        for (j=0; cmd[j]!=0; j++) {
                                printf("'%s' ", cmd[j]);
                        }
			printf("\n");
		}
		if (l->seq[1] != NULL) //meaning there is a 2nd command -> exitence of pipe
			execute_multiple_pipes(l, bg_process_list);
		else
			execute(l, bg_process_list);
	}
	free(bg_process_list);
}