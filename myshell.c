#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_UNDO 10

typedef enum { CMD_CD, CMD_MKDIR, CMD_TOUCH, CMD_MV, CMD_RM } CmdType;

typedef struct {
  CmdType type;
  char arg1[1024];
  char arg2[1024];
  time_t mtime;
  off_t size;
} UndoAction;

UndoAction undo_stack[MAX_UNDO];
int undo_head = 0;
int undo_tail = 0;
int undo_count = 0;
char trash_dir[1024];

void push_undo(UndoAction action) {
  if (undo_count == MAX_UNDO) {
    // Evict oldest permanently
    UndoAction oldest = undo_stack[undo_head];
    if (oldest.type == CMD_RM) {
      remove(oldest.arg2); // permanently delete from trash
    }
    undo_head = (undo_head + 1) % MAX_UNDO;
    undo_count--;
  }
  undo_stack[undo_tail] = action;
  undo_tail = (undo_tail + 1) % MAX_UNDO;
  undo_count++;
}

int pop_undo(UndoAction *action) {
  if (undo_count == 0)
    return 0;
  undo_tail = (undo_tail - 1 + MAX_UNDO) % MAX_UNDO;
  *action = undo_stack[undo_tail];
  undo_count--;
  return 1;
}

int get_file_stat(const char *path, struct stat *st) { return stat(path, st); }

// Helper to fully resolve paths (prevents issues with relative paths)
void resolve_path(const char *rel, char *abs_path, size_t size) {
  if (rel[0] == '/') {
    strncpy(abs_path, rel, size);
  } else {
    char cwd[1024];
    getcwd(cwd, sizeof(cwd));
    snprintf(abs_path, size, "%s/%s", cwd, rel);
  }
}

int main() {
  char buff[1024];
  char *args[100];

  // Initialize trash dir
  snprintf(trash_dir, sizeof(trash_dir), "%s/.myshell_trash", getenv("HOME"));
  mkdir(trash_dir, 0777);

  while (1) {
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
      printf("\033[1;32mmyshell\033[0m:\033[1;34m%s\033[0m$ ", cwd);
    } else {
      printf("myshell-> ");
    }

    if (fgets(buff, sizeof(buff), stdin) == NULL)
      break;
    buff[strcspn(buff, "\n")] = 0;

    int i = 0;
    char *token = strtok(buff, " ");
    while (token != NULL && i < 99) {
      args[i++] = token;
      token = strtok(NULL, " ");
    }
    args[i] = NULL;

    if (args[0] == NULL)
      continue;

    if (strcmp(args[0], "exit") == 0)
      break;

    // ------------- UNDO COMMAND -------------
    if (strcmp(args[0], "undo") == 0) {
      UndoAction action;
      if (!pop_undo(&action)) {
        printf("No actions to undo.\n");
        continue;
      }

      struct stat st;
      int conflict = 0;

      if (action.type == CMD_MKDIR || action.type == CMD_TOUCH) {
        if (get_file_stat(action.arg1, &st) == 0) {
          if (st.st_mtime != action.mtime || st.st_size != action.size)
            conflict = 1;
        } else {
          conflict = 1;
        }
        if (conflict) {
          printf("State Conflict: %s was modified externally.\n", action.arg1);
          continue;
        }
        remove(action.arg1);
        printf("Undo: Removed %s\n", action.arg1);
      } else if (action.type == CMD_MV) {
        if (get_file_stat(action.arg2, &st) == 0) {
          if (st.st_mtime != action.mtime || st.st_size != action.size)
            conflict = 1;
        } else {
          conflict = 1;
        }
        if (conflict) {
          printf("State Conflict: %s was modified externally.\n", action.arg2);
          continue;
        }
        rename(action.arg2, action.arg1);
        printf("Undo: Moved %s back to %s\n", action.arg2, action.arg1);
      } else if (action.type == CMD_RM) {
        if (rename(action.arg2, action.arg1) == 0) {
          printf("Undo: Restored %s\n", action.arg1);
        } else {
          perror("Undo: Failed to restore");
        }
      } else if (action.type == CMD_CD) {
        chdir(action.arg1);
        printf("Undo: Changed directory back to %s\n", action.arg1);
      }
      continue;
    }

    // ------------- CD COMMAND -------------
    if (strcmp(args[0], "cd") == 0) {
      char *path = args[1];
      if (path == NULL)
        path = getenv("HOME");
      else if (path[0] == '~') {
        static char temp[256];
        snprintf(temp, sizeof(temp), "%s%s", getenv("HOME"), path + 1);
        path = temp;
      }

      if (path == NULL) {
        fprintf(stderr, "myshell: cd: HOME not set\n");
      } else {
        char old_cwd[1024];
        getcwd(old_cwd, sizeof(old_cwd));
        if (chdir(path) == 0) {
          UndoAction action = {CMD_CD, "", "", 0, 0};
          strcpy(action.arg1, old_cwd);
          push_undo(action);
        } else {
          perror("myshell: cd failed");
        }
      }
      continue;
    }

    // ------------- RM COMMAND (INTERCEPT) -------------
    if (strcmp(args[0], "rm") == 0 && args[1] != NULL) {
      char trash_path[1024];
      snprintf(trash_path, sizeof(trash_path), "%s/%s_%ld", trash_dir, args[1],
               time(NULL));

      struct stat st;
      if (get_file_stat(args[1], &st) == 0) {
        if (rename(args[1], trash_path) == 0) {
          UndoAction action = {CMD_RM, "", "", 0, 0};
          resolve_path(args[1], action.arg1, sizeof(action.arg1));
          strcpy(action.arg2, trash_path);
          push_undo(action);
        } else {
          perror("rm failed (rename across filesystems or invalid path)");
        }
      } else {
        perror("rm failed");
      }
      continue;
    }

    // ------------- MV, MKDIR, TOUCH (PRE-EXECUTION) -------------
    UndoAction pending_action;
    int track_action = 0;
    struct stat pre_st;

    if (strcmp(args[0], "mv") == 0 && args[1] != NULL && args[2] != NULL) {
      if (get_file_stat(args[1], &pre_st) == 0) {
        pending_action.type = CMD_MV;
        resolve_path(args[1], pending_action.arg1, sizeof(pending_action.arg1));
        resolve_path(args[2], pending_action.arg2, sizeof(pending_action.arg2));
        track_action = 1;
      }
    } else if ((strcmp(args[0], "mkdir") == 0 ||
                strcmp(args[0], "touch") == 0) &&
               args[1] != NULL) {
      if (get_file_stat(args[1], &pre_st) !=
          0) { // Track only if it doesn't exist
        pending_action.type =
            (strcmp(args[0], "mkdir") == 0) ? CMD_MKDIR : CMD_TOUCH;
        resolve_path(args[1], pending_action.arg1, sizeof(pending_action.arg1));
        track_action = 1;
      }
    }

    // ------------- PROCESS EXECUTION -------------
    pid_t pid = fork();

    if (pid < 0) {
      perror("Fork failed");
    } else if (pid == 0) {
      if (execvp(args[0], args) == -1) {
        printf("Error: Command '%s' not found.\n", args[0]);
        exit(1);
      }
    } else {
      int status;
      waitpid(pid, &status, 0);

      // ------------- POST-EXECUTION (PUSH TO UNDO) -------------
      if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && track_action) {
        struct stat post_st;
        if (pending_action.type == CMD_MV) {
          if (get_file_stat(args[2], &post_st) == 0) {
            pending_action.mtime = post_st.st_mtime;
            pending_action.size = post_st.st_size;
            push_undo(pending_action);
          }
        } else {
          if (get_file_stat(args[1], &post_st) == 0) {
            pending_action.mtime = post_st.st_mtime;
            pending_action.size = post_st.st_size;
            push_undo(pending_action);
          }
        }
      }
    }
  }

  printf("Exiting myshell...\n");
  return 0;
}