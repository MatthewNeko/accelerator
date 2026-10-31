# vim: set ts=2 sw=2 tw=99 noet ft=python:
"""Idempotently apply a single patch to the breakpad working tree.

Why this exists
---------------
third_party/Patch runs `git reset --hard && git clean -fdx` inside
third_party/breakpad and then replays every file in patches/. The breakpad fork
(MatthewNeko/breakpad, branch `accelerator`) is periodically re-synced by
committing the contents of patches/ straight into the submodule - commit
8bb1668 "update" did exactly that for 0001..0006. Once that happens the pinned
submodule commit already contains the change, and `git apply` aborts:

    error: while searching for:
    BPLOG(ERROR) << "MinidumpModuleList could not read required module "
    ...
    error: patch failed: src/processor/minidump.cc:3198

`git apply --reject` makes it worse: it drops *.rej files into the tree and
still exits non-zero, so the whole build dies with a message that looks like the
patch content is stale when the reality is the opposite - the patch is already
in the tree.

So ask git what state the tree is in before touching it:
  * reverse-apply check passes -> patch already present, skip it
  * forward-apply check passes -> patch still needed, apply it
  * both fail                  -> the tree drifted away from what the patch
                                  expects (upstream re-based under us); fail
                                  loudly with git's own message so it is
                                  obvious which patch needs regenerating

Deliberately keeping patches/ wired into the build (instead of deleting them
once the fork carries the changes) means the build also stays correct if the
submodule is ever reset to a commit that does not include them yet.
"""

import os
import subprocess
import sys


def git_apply(flags, patch):
  proc = subprocess.Popen(['git', 'apply'] + flags + [patch],
                          stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)
  out, _ = proc.communicate()
  return proc.returncode, out.decode('utf-8', 'replace')


def main(argv):
  if not argv:
    sys.stderr.write('usage: apply_patches.py <patch> [<patch> ...]\n')
    return 2

  failures = 0
  for patch in argv:
    name = os.path.basename(patch)

    code, _ = git_apply(['--check', '--reverse'], patch)
    if code == 0:
      print('SKIP  %s (already applied)' % name)
      continue

    code, out = git_apply(['--check'], patch)
    if code != 0:
      sys.stderr.write('FAIL  %s (neither applied nor applicable)\n' % name)
      sys.stderr.write(out)
      failures += 1
      continue

    code, out = git_apply([], patch)
    if code != 0:
      sys.stderr.write('FAIL  %s (git apply failed)\n' % name)
      sys.stderr.write(out)
      failures += 1
      continue
    print('APPLY %s' % name)

  return 1 if failures else 0


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
