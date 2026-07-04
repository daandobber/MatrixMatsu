# Starting From The Original Template

Use the upstream template only when creating a completely fresh project:

```text
https://github.com/Nicolai-Electronics/tanmatsu-template
```

For MatrixMatsu work, prefer cloning the project repository instead:

```powershell
git clone https://github.com/daandobber/MatrixMatsu.git
```

After cloning, make sure these folders or tools are available before building:

- `esp-idf/`
- `esp-idf-tools/`
- `badgelink_v020/`
- ESP-IDF Python environment

This workspace already has those pieces locally. A clean clone may need the
same ESP-IDF and BadgeLink setup from the original Tanmatsu template docs.

