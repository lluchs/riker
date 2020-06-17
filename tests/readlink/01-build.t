Move to test directory
  $ cd $TESTDIR

Clean up any leftover state
  $ rm -rf .dodo
  $ rm -f output1 output2

Make sure link is a symlink to "HELLO"
  $ rm -f link
  $ ln -s HELLO link

Run the build
  $ $DODO --show
  dodo-launch
  Dodofile
  readlink link
  cat link
  cat: link: No such file or directory

Check the output
  $ cat output1
  HELLO
  $ cat output2

Run a rebuild, which should do nothing
  $ $DODO --show

Check the output again
  $ cat output1
  HELLO
  $ cat output2

Clean up
  $ rm -rf .dodo
  $ rm -f output1 output2