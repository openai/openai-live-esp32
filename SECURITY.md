# Credentials and device security

This experimental example stores a project API key and Wi-Fi profiles in device
flash so it can run without a separate backend. That is a development convenience,
not a production credential-provisioning design.

- Keep the API key file and Wi-Fi JSON outside the repository. The setup tool
  generates its credential header in an external cache with restrictive permissions.
- Locally built ELF files, firmware images, object files, and flash backups can
  contain those credentials. Do not upload them to issues, releases, or public storage.
- USB logs may contain transcripts, network names, session IDs, and API errors.
  Review and sanitize logs before sharing them.
- Screenshots may contain conversation text. Publish only captures you intend
  to share.
- Rotate credentials if a device or build artifact is shared unintentionally.
- For a product, design appropriate credential provisioning and device storage
  protection. The portable core leaves authentication to the application.

TLS certificate verification remains enabled. The HTTPS adapter does not follow
redirects while presenting the API key.

Do not post vulnerabilities together with working credentials or private data.
Use GitHub private vulnerability reporting if available for this repository.
