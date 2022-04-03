#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h> //termios, TCSANOW, ECHO, ICANON
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h> //for search file, dir 
#include <time.h>
const char *sysname = "shellfyre";

enum return_codes
{
	SUCCESS = 0,
	EXIT = 1,
	UNKNOWN = 2,
};

struct command_t
{
	char *name;
	bool background;
	bool auto_complete;
	int arg_count;
	char **args;
	char *redirects[3];		// in/out redirection
	struct command_t *next; // for piping
};

/**
 * Prints a command struct
 * @param struct command_t *
 */
void print_command(struct command_t *command)
{
	int i = 0;
	printf("Command: <%s>\n", command->name);
	printf("\tIs Background: %s\n", command->background ? "yes" : "no");
	printf("\tNeeds Auto-complete: %s\n", command->auto_complete ? "yes" : "no");
	printf("\tRedirects:\n");
	for (i = 0; i < 3; i++)
		printf("\t\t%d: %s\n", i, command->redirects[i] ? command->redirects[i] : "N/A");
	printf("\tArguments (%d):\n", command->arg_count);
	for (i = 0; i < command->arg_count; ++i)
		printf("\t\tArg %d: %s\n", i, command->args[i]);
	if (command->next)
	{
		printf("\tPiped to:\n");
		print_command(command->next);
	}
}

/**
 * Release allocated memory of a command
 * @param  command [description]
 * @return         [description]
 */
int free_command(struct command_t *command)
{
	if (command->arg_count)
	{
		for (int i = 0; i < command->arg_count; ++i)
			free(command->args[i]);
		free(command->args);
	}
	for (int i = 0; i < 3; ++i)
		if (command->redirects[i])
			free(command->redirects[i]);
	if (command->next)
	{
		free_command(command->next);
		command->next = NULL;
	}
	free(command->name);
	free(command);
	return 0;
}

/**
 * Show the command prompt
 * @return [description]
 */

int show_prompt()
{
	char cwd[1024], hostname[1024];
	gethostname(hostname, sizeof(hostname));
	getcwd(cwd, sizeof(cwd));
	printf("%s@%s:%s %s$ ", getenv("USER"), hostname, cwd, sysname);
	return 0;
}

/**
 * Parse a command string into a command struct
 * @param  buf     [description]
 * @param  command [description]
 * @return         0
 */
int parse_command(char *buf, struct command_t *command)
{
	const char *splitters = " \t"; // split at whitespace
	int index, len;
	len = strlen(buf);
	while (len > 0 && strchr(splitters, buf[0]) != NULL) // trim left whitespace
	{
		buf++;
		len--;
	}
	while (len > 0 && strchr(splitters, buf[len - 1]) != NULL)
		buf[--len] = 0; // trim right whitespace

	if (len > 0 && buf[len - 1] == '?') // auto-complete
		command->auto_complete = true;
	if (len > 0 && buf[len - 1] == '&') // background
		command->background = true;

	char *pch = strtok(buf, splitters);
	command->name = (char *)malloc(strlen(pch) + 1);
	if (pch == NULL)
		command->name[0] = 0;
	else
		strcpy(command->name, pch);

	command->args = (char **)malloc(sizeof(char *));

	int redirect_index;
	int arg_index = 0;
	char temp_buf[1024], *arg;

	while (1)
	{
		// tokenize input on splitters
		pch = strtok(NULL, splitters);
		if (!pch)
			break;
		arg = temp_buf;
		strcpy(arg, pch);
		len = strlen(arg);

		if (len == 0)
			continue;										 // empty arg, go for next
		while (len > 0 && strchr(splitters, arg[0]) != NULL) // trim left whitespace
		{
			arg++;
			len--;
		}
		while (len > 0 && strchr(splitters, arg[len - 1]) != NULL)
			arg[--len] = 0; // trim right whitespace
		if (len == 0)
			continue; // empty arg, go for next

		// piping to another command
		if (strcmp(arg, "|") == 0)
		{
			struct command_t *c = malloc(sizeof(struct command_t));
			int l = strlen(pch);
			pch[l] = splitters[0]; // restore strtok termination
			index = 1;
			while (pch[index] == ' ' || pch[index] == '\t')
				index++; // skip whitespaces

			parse_command(pch + index, c);
			pch[l] = 0; // put back strtok termination
			command->next = c;
			continue;
		}

		// background process
		if (strcmp(arg, "&") == 0)
			continue; // handled before

		// handle input redirection
		redirect_index = -1;
		if (arg[0] == '<')
			redirect_index = 0;
		if (arg[0] == '>')
		{
			if (len > 1 && arg[1] == '>')
			{
				redirect_index = 2;
				arg++;
				len--;
			}
			else
				redirect_index = 1;
		}
		if (redirect_index != -1)
		{
			command->redirects[redirect_index] = malloc(len);
			strcpy(command->redirects[redirect_index], arg + 1);
			continue;
		}

		// normal arguments
		if (len > 2 && ((arg[0] == '"' && arg[len - 1] == '"') || (arg[0] == '\'' && arg[len - 1] == '\''))) // quote wrapped arg
		{
			arg[--len] = 0;
			arg++;
		}
		command->args = (char **)realloc(command->args, sizeof(char *) * (arg_index + 1));
		command->args[arg_index] = (char *)malloc(len + 1);
		strcpy(command->args[arg_index++], arg);
	}
	command->arg_count = arg_index;
	return 0;
}

void prompt_backspace()
{
	putchar(8);	  // go back 1
	putchar(' '); // write empty over
	putchar(8);	  // go back 1 again
}

/**
 * Prompt a command from the user
 * @param  buf      [description]
 * @param  buf_size [description]
 * @return          [description]
 */
int prompt(struct command_t *command)
{
	int index = 0;
	char c;
	char buf[4096];
	static char oldbuf[4096];

	// tcgetattr gets the parameters of the current terminal
	// STDIN_FILENO will tell tcgetattr that it should write the settings
	// of stdin to oldt
	static struct termios backup_termios, new_termios;
	tcgetattr(STDIN_FILENO, &backup_termios);
	new_termios = backup_termios;
	// ICANON normally takes care that one line at a time will be processed
	// that means it will return if it sees a "\n" or an EOF or an EOL
	new_termios.c_lflag &= ~(ICANON | ECHO); // Also disable automatic echo. We manually echo each char.
	// Those new settings will be set to STDIN
	// TCSANOW tells tcsetattr to change attributes immediately.
	tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);

	// FIXME: backspace is applied before printing chars
	show_prompt();
	int multicode_state = 0;
	buf[0] = 0;

	while (1)
	{
		c = getchar();
		// printf("Keycode: %u\n", c); // DEBUG: uncomment for debugging

		if (c == 9) // handle tab
		{
			buf[index++] = '?'; // autocomplete
			break;
		}

		if (c == 127) // handle backspace
		{
			if (index > 0)
			{
				prompt_backspace();
				index--;
			}
			continue;
		}
		if (c == 27 && multicode_state == 0) // handle multi-code keys
		{
			multicode_state = 1;
			continue;
		}
		if (c == 91 && multicode_state == 1)
		{
			multicode_state = 2;
			continue;
		}
		if (c == 65 && multicode_state == 2) // up arrow
		{
			int i;
			while (index > 0)
			{
				prompt_backspace();
				index--;
			}
			for (i = 0; oldbuf[i]; ++i)
			{
				putchar(oldbuf[i]);
				buf[i] = oldbuf[i];
			}
			index = i;
			continue;
		}
		else
			multicode_state = 0;

		putchar(c); // echo the character
		buf[index++] = c;
		if (index >= sizeof(buf) - 1)
			break;
		if (c == '\n') // enter key
			break;
		if (c == 4) // Ctrl+D
			return EXIT;
	}
	if (index > 0 && buf[index - 1] == '\n') // trim newline from the end
		index--;
	buf[index++] = 0; // null terminate string

	strcpy(oldbuf, buf);

	parse_command(buf, command);

	// print_command(command); // DEBUG: uncomment for debugging

	// restore the old settings
	tcsetattr(STDIN_FILENO, TCSANOW, &backup_termios);
	return SUCCESS;
}

int process_command(struct command_t *command);
void file_printer(char *file_list[], size_t size);

char history_path[1024];

int main()
{
	strcat(getcwd(history_path, sizeof(history_path)), "/history.txt"); //initialize path
	while (1)
	{
		struct command_t *command = malloc(sizeof(struct command_t));
		memset(command, 0, sizeof(struct command_t)); // set all bytes to 0

		int code;
		code = prompt(command);
		if (code == EXIT)
			break;

		code = process_command(command);
		if (code == EXIT)
			break;

		free_command(command);
	}

	printf("\n");
	return 0;
}

char* set_path(char *command);

int open_file(char *name) { //this function is helper for filesearch()

	pid_t pid = fork();
	if (pid == 0) {
		char *buf[] = {"xdg-open", name, NULL};
		char *path = set_path("xdg-open");
		execv(path, buf);
		free(path);
	
	} else {
		wait(NULL);
		//kill(pid, SIGKILL);
	}


	return SUCCESS;
}

void file_properties() {

	char *input = (char*)malloc(100);
	struct stat *buf = malloc(sizeof(struct stat));
	printf("Enter file name: ");
	fgets(input, 100, stdin);
	input[strlen(input)-1]= '\0';
	if(stat(input, buf) == 0) {
		struct tm date;
		struct tm date2;
		printf("File access permisson: ");
		if (buf->st_mode & R_OK) {
			printf("read");
		}

		if (buf->st_mode & W_OK) {
                        printf("write");
                }

		if (buf->st_mode & X_OK) {
                        printf("execute");
                }

		printf("\nFile size: %ld\n", buf->st_size);
		
		date = *(gmtime(&buf->st_ctime));
		printf("Created date: %d-%d-%d, time: %d:%d:%d\n", date.tm_mday, date.tm_mon, date.tm_year+1900,
				date.tm_hour, date.tm_min, date.tm_sec);

		date2 = *(gmtime(&buf->st_mtime));
                printf("Modified date: %d-%d-%d, time: %d:%d:%d\n", date2.tm_mday, date2.tm_mon, date2.tm_year+1900, 
				date2.tm_hour, date2.tm_min, date2.tm_sec);
	} else {
		printf("File can not find.\n");
	
	}

	//free(buf);
	free(input);

}

void file_search(char *fileName, char *secondCommand, char *dirCommand, char *thirdCommand) 
{	
	DIR *folder;
	struct dirent *entry;
	folder = opendir(dirCommand); //current folder

	if (folder != NULL) {
		 while ((entry = readdir(folder))) { //this loop will be worked equal to NULLi
                        if (secondCommand != NULL && secondCommand != "dirl") {
                                if (strcmp(entry->d_name, ".") == 0) {
                                      file_search(fileName, "dirl", "..", thirdCommand); //recursion
                                } else if (strstr(entry->d_name, "..")) {
                                      file_search(fileName, "dirl", "...", thirdCommand);
                                }
                        }

                        if (strstr(entry->d_name, fileName)) { //print and open method
				if (secondCommand == "dirl") {
				//	printf("\n Third : %s \n", thirdCommand);
					if (thirdCommand == "r") {
						printf("  ./dirl/%s\n", entry->d_name);
					} else if (thirdCommand	== "o" ) {
						open_file(entry->d_name);
					} else if (thirdCommand == "ro") {
						printf("  ./dirl/%s\n", entry->d_name);
						open_file(entry->d_name);
					}
				} else { 
					if (thirdCommand == "r") {
						printf("  ./%s\n", entry->d_name);
					} else if (thirdCommand == "o" ) {
                                                open_file(entry->d_name);
                                        } else if (thirdCommand == "ro") {
                                                printf("  ./%s\n", entry->d_name);
                                                open_file(entry->d_name);
                                        }
				}
                        }
                }
	}
	closedir(folder);
}

void pwd() // extra basic 
{
	char cwd[1024];
        getcwd(cwd, sizeof(cwd));
        printf("%s\n", cwd);
}

void ls() // extra  basic 
{
	DIR *folder;
        struct dirent *entry;
        folder = opendir("."); 

        if (folder != NULL) {
                 while ((entry = readdir(folder))) { //this loop will be worked equal to NULL
			 if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) { //print method
                               printf("  %s", entry->d_name);
                        }
                 }
		 printf("\n");
        }
        closedir(folder);
}

/*
 *this function is helper for cdh()
 *this function chechk history.txt length. If length is greater than 10 lines, delete lines after 10 th line.
 */
void check_length_file() { 
	FILE *history = fopen(history_path, "r");
	int line_count = 0;
	char line[500];
	while (fgets(line, 500, history)) {
		line_count++;
	}
	fclose(history);
	if (line_count>9) {
		char text_array[10][500];
		FILE *history = fopen(history_path, "r");
    		char line2[500];
		int temp = 0;
		int i=0;
        	while (fgets(line2, 500, history)) {
                	temp++;
			if (line_count-temp < 10) {
				strcpy(text_array[i], line2);
				i++;
			}
        	}
		fclose(history);
		FILE *history_write = fopen(history_path, "w");
		int j=0;
		for (j=0; j<10; j++) {
			fprintf(history_write, "%s", text_array[j]);
		}
		fclose(history_write);
	}
}

/*
 * this function is a helper for cdh command.
 * print history text after calling cdh command
 */
void print_in_file() 
{
	FILE *history = fopen(history_path, "a+"); //append mode
	char cwd[1024];
        getcwd(cwd, sizeof(cwd));
	fprintf(history, "%s\n", cwd);
	fclose(history); 
	check_length_file();
}


void cdh() 
{	
	FILE *history = fopen(history_path, "r");
	char line[500];
	int size = 0;
        while (fgets(line, 500, history)) {
        	size++;
        }
	char text_array[size][500];
	int i=0;
	fseek(history, 0, SEEK_SET);
	while (fgets(line, 500, history)) {
		strcpy(text_array[i], line);
		i++;
	}
	fclose(history);
	int j=0;
	for (j=0; j<size; j++) {
		printf("%c  %d)  %s", 96+size-j,  size-j, text_array[j]);
	}

	printf("Select directory by letter or number: ");
	char input[15];
	fgets(input, 15, stdin);
	int selected = 0;
	input[strlen(input)-1] = '\0'; 
	if (input[0]>96 && input[0]<97+size) {
		selected = input[0]-96;
		char *directory = text_array[size-selected];
	       	directory[strlen(directory)-1] = '\0'; 	
		chdir(directory);
	} else if (input[0]>0 && input[0]>size+1) {
	        selected = atoi(input);
	 	char *directory = text_array[size-selected];
                directory[strlen(directory)-1] = '\0';
                chdir(directory);	
	}
	//printf("****%d\n", selected);

}

/*
 * this function is a helper for basic commands 
 * this function is a helper for open_file funtion
 */
char* set_path(char *command) {
	char *path = malloc(1024);
	strcat(path, "/usr/bin/"); //path adress
	strcat(path, command);

	struct stat *buf = malloc(sizeof(struct stat));
	int check_path = stat(path, buf); //if path valid or not 

	if (check_path == 0) {
		free(buf);
		return path;
	}

	free(path);
	free(buf);
	return NULL;
}

int process_command(struct command_t *command)
{
	int r;

	if (strcmp(command->name, "") == 0)
		return SUCCESS;

	if (strcmp(command->name, "exit") == 0)
		return EXIT;

	if (strcmp(command->name, "cd") == 0)
	{
		if (command->arg_count > 0)
		{
			r = chdir(command->args[0]);
			if (r == -1) {
				printf("-%s: %s: %s\n", sysname, command->name, strerror(errno));
			}else {
				//printf("\n***ok***\n");
				//strcat(getcwd(history_path, sizeof(history_path)), "history.txt");
				print_in_file();
			}
			return SUCCESS;
		}
	}

	// TODO: Implement your custom commands here

	if (strcmp(command->name, "take") == 0)
	{
	//	takeFunc();
		return SUCCESS;
	}

	if (strcmp(command->name, "filesearch") == 0)
        {
		if (command->arg_count == 1) {
			file_search(command->args[0], NULL, ".", "r");

		} else if (command->arg_count == 2) {
			if (strcmp(command->args[0], "-r") == 0) {
				file_search(command->args[1], "current", ".", "r");
			} else if (strcmp(command->args[0], "-o") == 0) {
                                file_search(command->args[1], "current", ".", "o");
                        } else {
				printf("Command not found.\n");
			}
                } else if (command->arg_count == 3) {
			if (((strcmp(command->args[0], "-r") == 0) && (strcmp(command->args[1], "-o") == 0)) ||
				((strcmp(command->args[0], "-o") == 0) && (strcmp(command->args[1], "-r") == 0))) {
				file_search(command->args[2], "current", ".", "ro");		
			} else {
                                printf("Command not found.\n");
                        }
                } else {
			printf("Please enter valid input.\n");
		}
		return SUCCESS;
        }

	if (strcmp(command->name, "cdh") == 0)
        {
		cdh();
		return SUCCESS;
        }

	if (strcmp(command->name, "joker") == 0)
        {	
		//joker();
		return SUCCESS;
        }


	if (strcmp(command->name, "fileproperties") == 0)
        {
                file_properties();
                return SUCCESS;
        }


	if (strcmp(command->name, "manual") == 0)
        {
		if (strcmp(command->args[0], "pwd") == 0) {
			pwd();
		} else if (strcmp(command->args[0], "ls") == 0) {
                        ls();
                }
                return SUCCESS;
        }

	if (strcmp(command->name, "basiccalculator") == 0)
        {

                printf("order for op:  basiccalculator <number1> <operator> <number2>\n");
                        printf("operators : +, -, *, /.\n");

                char op = command->args[2];


  double number1= atoi(command->args[1]);
  double number2= atoi(command->args[3]);


  switch (op) {
    case '+':
      printf("%.1lf + %.1lf = %.1lf", number1, number2, number1 + number2);
      break;
    case '-':
      printf("%.1lf - %.1lf = %.1lf", number1, number2, number1 - number2);
      break;
    case '*':
      printf("%.1lf * %.1lf = %.1lf", number1, number2, number1 * number2);
      break;
    case '/':
      printf("%.1lf / %.1lf = %.1lf", number1, number2, number1 / number2);
      break;
    // operator doesn't match any case constant
    default:
      printf("Error! operator is not correct");
        }
	}

	pid_t pid = fork();

	if (pid == 0) // child
	{
		// increase args size by 2
		command->args = (char **)realloc(
			command->args, sizeof(char *) * (command->arg_count += 2));

		// shift everything forward by 1
		for (int i = command->arg_count - 2; i > 0; --i)
			command->args[i] = command->args[i - 1];

		// set args[0] as a copy of name
		command->args[0] = strdup(command->name);
		// set args[arg_count-1] (last) to NULL
		command->args[command->arg_count - 1] = NULL;

		/// TODO: do your own exec with path resolving using execv()i
		char *local_path = set_path(command->name);// "/usr/bin/pwd";
		
		if (local_path != NULL) {
			//printf("%s\n", local_path);
			execv(local_path, command->args);
		}						
	       	else {
		//	printf("-%s: %s: %s\n", sysname, command->name, strerror(errno));
		}
				
		exit(0);
	}
	else
	{
		/// TODO: Wait for child to finish if command is not running in background
		if(!command->background) { 
			wait(NULL);
		}
		return SUCCESS;
	}

	printf("-%s: %s: command not found\n", sysname, command->name);
	return UNKNOWN;
}

void file_printer(char *file_list[], size_t size) {

	int i,j;

    for (i = 0; i < size; i++) {

        if (strcmp(file_list[i], "") == 0) {

            j= i - 1;

            while (index >= 0) {

                printf("\t%s\n", file_list[j]);
                j--;
            }

            printf("\nAll files printed.");

            break;
        }
    }
}
