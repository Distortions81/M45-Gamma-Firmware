# Repository guidance

- Read `README.md` at the start of a new session.
- Do not create branches or push changes without explicit user permission.
- When adding or expanding web pages, HTTP routes, or JSON fields, audit every
  related fixed-size capacity. In particular, check HTTP handler slots, request
  and response buffers, task stacks, lwIP socket concurrency, generated web
  assets, and application-partition headroom. Prefer compile-time guards where
  capacities can be related mechanically, and run a full firmware build.
