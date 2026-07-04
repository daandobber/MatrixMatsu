# MatrixMatsu Starter Kit

This folder is the short version of the project context for a fresh AI session
or a new developer.

Read these files first:

- `AGENT_PROMPT.md`: pasteable prompt for a new AI session.
- `COMMANDS.md`: exact build, install, package, and git commands.
- `PROJECT_MAP.md`: where important code and scripts live.
- `HARDWARE.md`: Tanmatsu hardware specs relevant to app development.
- `bootstrap-matrixmatsu.ps1`: one-command bootstrap for a clean machine or
  fresh folder.

The root `AGENTS.md` contains the same operational rules in a form that coding
agents commonly discover automatically.

Quick start from a folder where you want the project cloned:

```powershell
New-Item -ItemType Directory -Force MatrixMatsu-bootstrap
Set-Location MatrixMatsu-bootstrap
Invoke-WebRequest https://raw.githubusercontent.com/daandobber/MatrixMatsu/main/starter-kit/bootstrap-matrixmatsu.ps1 -OutFile bootstrap-matrixmatsu.ps1
powershell -ExecutionPolicy Bypass -File .\bootstrap-matrixmatsu.ps1 -SetupBadgeLink -SetupSdk -CloneTemplateToo
```

If this starter kit already exists locally:

```powershell
powershell -ExecutionPolicy Bypass -File .\starter-kit\bootstrap-matrixmatsu.ps1 -SetupBadgeLink
```

Use `-SetupSdk` too when ESP-IDF is not installed yet. Use `-CloneTemplateToo`
when you also want a separate checkout of the original Nicolai template next to
MatrixMatsu.
