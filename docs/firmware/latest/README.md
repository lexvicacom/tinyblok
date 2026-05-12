# tinyblok firmware assets

The ESP Web Tools manifest at `docs/manifest.json` expects release/deploy
automation to place these files here on GitHub Pages:

- `bootloader.bin`
- `partition-table.bin`
- `tinyblok.bin`

They are not committed to the repository. Keeping them on the same Pages origin
as `install.html` avoids browser CORS and redirect problems during flashing.
