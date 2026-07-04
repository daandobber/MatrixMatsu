# Starting From The Original Template

Use the upstream template only when creating a completely fresh project:

```text
https://github.com/Nicolai-Electronics/tanmatsu-template
```

For MatrixMatsu work, prefer cloning the project repository instead:

```powershell
git clone https://github.com/daandobber/MatrixMatsu.git
```

Or use the bootstrap script from this starter kit:

```powershell
New-Item -ItemType Directory -Force MatrixMatsu-bootstrap
Set-Location MatrixMatsu-bootstrap
Invoke-WebRequest https://raw.githubusercontent.com/daandobber/MatrixMatsu/main/starter-kit/bootstrap-matrixmatsu.ps1 -OutFile bootstrap-matrixmatsu.ps1
powershell -ExecutionPolicy Bypass -File .\bootstrap-matrixmatsu.ps1 -SetupBadgeLink -SetupSdk -CloneTemplateToo
```

When the starter kit already exists locally:

```powershell
powershell -ExecutionPolicy Bypass -File .\starter-kit\bootstrap-matrixmatsu.ps1 -SetupBadgeLink -SetupSdk -CloneTemplateToo
```

That script:

- clones MatrixMatsu
- adds the MatrixMatsu project remote as `matrixmatsu`
- adds the Nicolai template remote as `template`
- optionally clones the Nicolai template as a separate sibling folder
- optionally downloads BadgeLink tooling
- optionally downloads and installs ESP-IDF

After cloning, make sure these folders or tools are available before building:

- `esp-idf/`
- `esp-idf-tools/`
- `badgelink_v020/`
- ESP-IDF Python environment

This workspace already has those pieces locally. A clean clone may need the
same ESP-IDF and BadgeLink setup from the original Tanmatsu template docs.
