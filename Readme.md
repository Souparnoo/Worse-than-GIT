# Worse-Than-Git: Quick Start & Testing Guide
WTGit is a lightweight command-line version control system inspired by Git, developed in C++17. It enables users to initialize repositories, stage files, create commits, view commit history, inspect repository status, restore previous versions using checkout, and undo changes through revert operations. The project uses OpenSSL for secure SHA-1 hash generation to uniquely identify commits and file objects, while maintaining repository metadata and snapshots in a custom storage structure. WTGit provides an easy-to-understand implementation of core version control concepts, making it a practical educational tool for learning how distributed version control systems manage file tracking, commits, and repository history.
## Prerequisites

* Linux (Ubuntu/Debian recommended)
* g++ with C++17 support
* OpenSSL development libraries

Install dependencies:

```bash
sudo apt update
sudo apt install build-essential libssl-dev
```

## Build WTGit

From the project root:

```bash
mkdir -p ~/wtgit/bin
make
```

Or use the provided build script:

```bash
cd scripts
chmod +x build.sh wtgit.sh
./build.sh
source ~/.bashrc
```

Verify the installation:

```bash
wtgit
```

You should see the WTGit usage/help menu.

---

## Creating a Repository

Create a test directory:

```bash
mkdir ~/wtgit-test
cd ~/wtgit-test
```

Initialize the repository:

```bash
wtgit init
```

---

## Basic Workflow

### 1. Create a file

```bash
echo "Hello World" > hello.txt
```

### 2. Check repository status

```bash
wtgit status
```

### 3. Stage the file

```bash
wtgit add hello.txt
```

### 4. Commit the changes

```bash
wtgit commit "Initial commit"
```

### 5. View commit history

```bash
wtgit log
```

---

## Testing File Updates

Modify the file:

```bash
echo "Second line" >> hello.txt
```

Check status:

```bash
wtgit status
```

Stage and commit:

```bash
wtgit add hello.txt
wtgit commit "Updated hello.txt"
```

View history:

```bash
wtgit log 
```

Expected output:

```text
<latest_hash>  Updated hello.txt
<previous_hash>  Initial commit
```

---

## Testing Checkout

Restore an older version:

```bash
wtgit checkout <commit_hash>
cat hello.txt
```

Restore the latest version:

```bash
wtgit checkout <latest_commit_hash>
cat hello.txt
```

The file contents should match the selected commit.

---

## Testing Revert

Undo a previous commit:

```bash
wtgit revert <commit_hash>
```

Check the repository status and commit history:

```bash
wtgit status
wtgit log
```

---

## Useful Commands

| Command                  | Description                       |
| ------------------------ | --------------------------------- |
| `wtgit init`             | Initialize a repository           |
| `wtgit add <file>`       | Stage files                       |
| `wtgit status`           | View staged and unstaged changes  |
| `wtgit commit "message"` | Create a commit                   |
| `wtgit log`              | Display commit history            |
| `wtgit checkout <hash>`  | Restore a previous commit         |
| `wtgit revert <hash>`    | Revert a commit                   |
| `wtgit resolve`          | Clear pending merge-conflict lock |
