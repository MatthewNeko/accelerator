# vim: set ts=2 sw=2 tw=99 noet ft=python:
import subprocess, sys  

argv = sys.argv[3:]
cwd = sys.argv[1]

# The command is handed to a shell ('&&' is used to chain git invocations), so
# arguments are joined rather than exec'd. Anything containing whitespace has to
# be quoted or the shell splits it - which is why building from a path with a
# space in it used to fail with "can't open patch 'H:/code/CSGO'".
def quote(arg):
  return '"%s"' % arg if any(c.isspace() for c in arg) else arg

cmd = ' '.join(quote(arg) for arg in argv)

print('CMD: ' + cmd)
print('CWD: ' + cwd)
print('OUT: ' + sys.argv[2])
open(sys.argv[2], 'a').close()

args = { 'args': cmd,
          'stdout': subprocess.PIPE,
          'stderr': subprocess.PIPE,
          'shell':  True,
          'cwd': cwd }
p = subprocess.Popen(**args)
stdout, stderr = p.communicate()
print(stdout.decode("utf-8"))
if p.returncode != 0:
  print(stderr.decode("utf-8"))
sys.exit(p.returncode)
