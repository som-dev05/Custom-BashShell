# Custom C Shell

A simple UNIX shell written in C that supports standard external commands, built-in directory navigation, and a unique **Temporal Safety Net** (`undo` feature).

## Features

- **Standard Execution**: Executes standard Linux commands (`ls`, `grep`, `pwd`) by forking child processes.
- **Built-in Navigation**: Custom implementation of `cd`, including `~` (tilde) expansion and no-argument home navigation.
- **Temporal Safety Net (`undo`)**: The shell tracks the state of your last 10 modifying commands and allows you to reverse them sequentially.

### Supported Undo Operations:
- `rm`: Intercepts file deletion, temporarily moving files to a hidden trash bin (`~/.myshell_trash`). Typing `undo` restores the file with all metadata intact. If the history buffer exceeds 10 commands, the oldest deleted files drop off the stack and are permanently purged.
- `mkdir` & `touch`: Remembers created files/folders and removes them on `undo`, provided they haven't been modified externally.
- `mv`: Remembers the original and new locations, moving the file back to its source on `undo`.
- `cd`: Returns you to your previous working directory.

*Note: The shell actively protects you from data corruption by checking the file's modified time (`mtime`) and size before undoing operations. If an external process has modified the file since the command was run, the shell will report a "State Conflict" and refuse to reverse the action.*

## How to Compile and Run

Make sure you have a C compiler like GCC installed on your Linux environment.

```bash
# Compile the shell
gcc myshell.c -o myshell

# Run the shell
./myshell
```

## Collaboration

This project uses Git for version control. To start contributing, clone the repository, create a branch for your feature, and submit a pull request!
