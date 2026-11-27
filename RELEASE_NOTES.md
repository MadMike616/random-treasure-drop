## Changes

- Successful first-run file setup now records a completion token in the plugin TOML and is skipped on later launches.
- Existing installations and user settings are preserved; unsuccessful setup can be retried.
- Later launches use the installed tables without repeating base-file copies.
