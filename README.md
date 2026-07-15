WARNING: THIS IS AN EARLY BUILD AND IS NOT COMPLETE, PLEASE DO NOT USE TO REPLACE BASH (as of v1.0) 
UPDATE: OpSim is the newest edition.

# Super Duper Shell (sdsh)

**Super Duper Shell (sdsh)** is a lightweight, custom command-line shell written in C. Specially tuned for Ubuntu and KDE Neon (Konsole terminal), it acts as an educational wrapper that communicates directly with the Linux kernel using POSIX APIs like `fork()`, `execvp()`, and `waitpid()`. 

It features an intuitive translation layer that replaces standard cryptic Linux commands with highly memorable alternatives, includes native built-ins, handles custom scripting, and protects itself from accidental closures via `Ctrl+C`.

---

## ✨ Features

* **Vibrant Prompt:** Displays a colorful `[sdsh] user@machine:cwd$` dynamic interface using VT100/ANSI escape codes.
* **Command Translation Layer:** Converts custom human-friendly instructions into real underlying binaries (e.g., `nuke` becomes `rm`).
* **Smart Privilege Escalation:** The `superuser` prefix maps directly to `sudo` and smoothly expands compound aliases (e.g., `superuser grab vim` safely translates to `sudo apt install vim`).
* **Signal Protection:** Intercepts `Ctrl+C` (`SIGINT`) to clear the current line and re-render a fresh prompt instead of crashing your active shell.
* **Scripting Engine:** Supports executing standard text files line-by-line using the `.sdsh` file extension.

---

## 📖 Command Reference

If a command is not listed below, `sdsh` automatically passes it through to your system's environmental `$PATH` via `execvp()`. This means core tools like `grep`, `nano`, `git`, and `ssh` will continue to function exactly as expected.

### 📁 File & Directory Operations
| sdsh Command | Target Linux Binary | Description |
| :--- | :--- | :--- |
| `list <path>` | `ls` | Lists directory contents. |
| `listdat` | `lsblk` | Lists block devices and storage information. |
| `newfolder <name>` | `mkdir` | Generates a new directory. |
| `nuke <target>` | `rm` | Deletes files or directories. |
| `clone <src> <dest>` | `cp` | Copies a file or folder. |
| `shift <src> <dest>` | `mv` | Moves or renames a file or folder. |
| `peek <file>` | `cat` | Concatenates and displays file contents. |
| `spark <file>` | `touch` | Generates a blank file or updates timestamps. |

### ⚙️ System & Package Management
| sdsh Command | Target Linux Binary | Description |
| :--- | :--- | :--- |
| `grab <package>` | `apt install` | Downloads and installs a system package. |
| `refresh` | `apt update` | Pulls down updated repository listings. |
| `evolve` | `apt upgrade` | Upgrades all installed system components. |
| `revive` | `reboot` | Safely restarts the machine. |
| `sleep` | `poweroff` | Shuts down the machine completely. |

### 🔐 Privilege Escalation
| sdsh Command | Target Linux Binary | Example Execution |
| :--- | :--- | :--- |
| `superuser <cmd>` | `sudo <cmd>` | Runs tasks with root access. Nested alias tracking handles syntax like `superuser grab htop` flawlessly. |

### 🛠 Native Built-ins
These special items run natively inside the primary shell process, completely avoiding the overhead of secondary process splitting:
* `about`: Renders an stylized ASCII text banner alongside a cheat-sheet of available commands.
* `whereami`: Prints out your current physical working directory.
* `cls`: Resets and clears your current terminal display panel.
* `cd <path>`: Navigates your terminal shell into a different folder.
* `exit`: Safely tears down the runtime ecosystem and closes out the program.

---

## 🚀 Compilation & Installation

To build `sdsh` from its source code, ensure you have a standard C compiler toolkit (`gcc` or `clang`) deployed to your Unix operating system environment.

### 1. Compile the Binary
Run the compiler from your terminal window, passing standard safety flags to verify optimization constraints:
```bash
gcc -Wall -Wextra -o sdsh sdsh.c
