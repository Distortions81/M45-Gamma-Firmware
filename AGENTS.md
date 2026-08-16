# Repository guidance

- Read `README.md` at the start of a new session.
- Do not create branches or push changes without explicit user permission.
- When adding or expanding web pages, HTTP routes, or JSON fields, audit every
  related fixed-size capacity. In particular, check HTTP handler slots, request
  and response buffers, task stacks, lwIP socket concurrency, generated web
  assets, and application-partition headroom. Prefer compile-time guards where
  capacities can be related mechanically, and run a full firmware build.
- Fixed-size JSON and HTTP buffers are acceptable. Size them to a power of two
  with roughly twice the realistically required capacity instead of maintaining
  narrowly fitted, build-specific limits that need adjustment whenever a field
  is added.
- Sanitize hashrate and domain telemetry before exposing it: non-finite or
  negative rates, and rates above 1,000 TH/s, are invalid and must be reported
  as zero. Likewise, bounded telemetry percentages must report zero when they
  are non-finite, negative, or above their valid maximum.
