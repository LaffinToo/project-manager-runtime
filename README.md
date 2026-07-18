# Project Management Runtime Tool (PM)

PM is a lightweight C-based workspace switcher for mobile and desktop development. Its purpose is simple: keep a project in a persistent vault, load it into a temporary active workspace when you need to edit or inspect it, and then commit or restore changes back into the vault.

It is especially useful when you want a clean, reliable way to work across Android/Termux, Windows/WSL, or any setup where you need to move a project between a stable storage location and a temporary working directory.

A typical session looks like this:

```bash
project init myapp
project load myapp
project commit src/project.c
project status
project sync
```

This tool was designed around a very practical workflow:
- you may be working from a location that is easy to reach from Android apps or a mobile file browser,
- you may want to use Termux or a shell session for building and testing,
- you may also be working in Windows with WSL and want a clean way to keep a project in a stable vault while switching into a temporary Linux workspace,
- and you want a safe way to move source files between a long-lived storage location and a temporary working directory.

---

## Why this tool exists

On Android or in a mobile-first setup, the usual workflow can be awkward:
- the shell environment is temporary,
- files may be scattered across folders that are hard to manage,
- and you want a clean separation between a stable source-of-truth vault and a working directory.

PM solves that by giving you a small command-line workflow:
- keep project data in a persistent vault,
- load it into an active workspace when needed,
- commit individual files back into the vault,
- restore or back up a snapshot when required.

It is especially useful when you want to edit files from a Documents-style folder and then work with them from Termux.

---

## How it works

PM relies on two environment variables:

```bash
export PM_STORAGE_DIR="/path/to/your/persistent/storage/projects"
export PM_ACTIVE_DIR="/path/to/your/active/workspace"
```

- PM_STORAGE_DIR is the persistent vault root.
- PM_ACTIVE_DIR is the temporary workspace where the project is loaded for editing.

When you initialize a project, PM creates a project folder under the storage root and sets up a stable truth area for the project. It also keeps a project.config file in the active workspace so the tool can remember which project is currently loaded.

The tool can also recognize older or alternate layout forms such as:
- project_root/truth
- project_root/vault
- project_root/files
- project_root (fallback)

If it discovers such a layout, it can migrate the existing contents into the canonical truth structure automatically.

---

## Quick start

### 1. Install dependencies

On Android/Termux:

```bash
pkg update
pkg install tcc make libarchive
```

On Debian/Ubuntu/WSL:

```bash
sudo apt update
sudo apt install build-essential libarchive-dev
```

### 2. Build and install

```bash
make clean
make
make install
```

The binary will be installed to ~/.local/bin/project.

### 3. Set your environment

Add these exports to ~/.bashrc, ~/.zshrc, or your shell profile:

```bash
export PM_STORAGE_DIR="/path/to/your/persistent/storage/projects"
export PM_ACTIVE_DIR="/path/to/your/active/workspace"
```

### 4. Initialize a project

```bash
project init myapp
```

This creates the storage layout for the project under the storage root.

### 5. Load the project into your active workspace

```bash
project load myapp
```

The tool copies the project contents into your active workspace so you can edit them there.

### 6. Commit files back to the vault

```bash
project commit path/to/file
```

This writes the file into the truth vault under the same relative path structure it had in the active workspace.

### 7. Check the current status

```bash
project status
```

This shows whether the active workspace differs from the stored truth state and reminds you about uncommitted files.

### 8. Restore or reset the workspace

```bash
project sync
```

This resets the active workspace to the stored truth state.

```bash
project unload
```

This packages the current workspace state and clears the active area.

---

## Command reference

### Quick command list

- project init <ProjectName> — creates the project vault folders and initial config structure.
- project list — lists the available projects in the storage vault.
- project status — shows whether the active workspace differs from the stored truth state.
- project ignore <FileName/Pattern> — adds a file or pattern to the ignore list.
- project load <ProjectName> — loads a project from the persistent vault into the active workspace.
- project commit <FileName> — commits a file into the stable truth vault.
- project sync — resets the active workspace to match the stored truth state.
- project unload — archives the active workspace state and clears the temporary workspace.
- project backup — creates a compressed backup snapshot of the current truth vault.
- project backup list — shows the available backup snapshots.
- project backup restore <Timestamp_ID> — restores an older vault snapshot.

### Project setup

```bash
project init <ProjectName>
```
Creates the project vault folders and initial config structure.

### Load and unload

```bash
project load <ProjectName>
```
Loads the project from the persistent vault into the active workspace.

```bash
project unload
```
Archives the active workspace state and clears the temporary workspace.

### File tracking

```bash
project commit <FileName>
```
Commits a file into the stable truth vault.

```bash
project ignore <FileName/Pattern>
```
Adds a pattern to the ignore list so it is skipped from tracking.

```bash
project status
```
Shows the current project context and whether the workspace has drifted from the vault.

### Recovery and backup

```bash
project backup
```
Creates a compressed backup snapshot of the current truth vault.

```bash
project backup list
```
Shows the available backup snapshots.

```bash
project backup restore <Timestamp_ID>
```
Restores an older vault snapshot.

```bash
project sync
```
Resets the active workspace to match the stored truth state.

---

## Typical usage example

A common mobile workflow looks like this:

1. Put your persistent storage somewhere stable, such as a folder on internal storage or a synced location.
2. Set PM_STORAGE_DIR to that location.
3. Set PM_ACTIVE_DIR to a folder you can edit from a mobile file browser or a shell session.
4. Run project init <ProjectName> once.
5. Use project load <ProjectName> to bring the project into the active folder.
6. Edit files in the active folder as needed.
7. Use project commit <FileName> to save important changes back into the vault.
8. Use project sync or project unload when you want to reset or clean up the active workspace.

That gives you a simple way to keep the project state organized while still working in a flexible location.
