- Study `CONTRIBUTING.md` to get all the key information on how to build
  NetworkManager and write patches.
- If the `origin` remote is pointed to GitHub, stop immediately and
  remind the user that NetworkManager upstream development happens on
  https://gitlab.freedesktop.org.
- Ensure you have a `build/` meson directory that successfully builds
  before commencing any work.
- Before committing anything, make sure your code builds successfully,
  passes all tests and is properly formatted.
- Unless your work depends on previous unmerged work, make sure you are
  on a new clean branch that is based on `main`.
- If possible, update stale comments that become out of date due to your
  changes, instead of outright removing them. This obviously does not
  apply to TODO or FIXMEs that have been resolved and can be removed.
- If the system has a installation of `podman`, you can use
  `./contrib/scripts/nm-code-format-container.sh` to get a consistent
  formatting of your changes. Otherwise, use `nm-code-format.sh` as
  described in `CONTRIBUTING.md`.
- Commit titles are written in the style of
  `<area>: <short description>`. They should fit under 72 characters.
  They should be concise and descriptive.
- Please study existing commit messages to get an idea of how to write a
  good commit message. If you edit a particular file, study its git
  history to see how it was previously commented.
- After making a commit, use `checkpatch.pl` as described by the
  contributing document to ensure your patch is valid.
- Unit tests live in this repository. Integration tests are in a
  separate repository called `NetworkManager-ci`. You may have a clone
  of it in ../NetworkManager-ci. Before making any changes to that repo,
  please first study its contents and contributing guidelines.
  When making a significant change, it might be good to add integration
  tests for it. Whether to make unit tests or integration tests should
  be decided on a case-by-case basis, depending on if the
  surrounding/relevant code is already unit tested or not.
