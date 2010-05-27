# -*- coding: utf-8 -*-
# Copyright 2010, Google Inc.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#     * Redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above
# copyright notice, this list of conditions and the following disclaimer
# in the documentation and/or other materials provided with the
# distribution.
#     * Neither the name of Google Inc. nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""Script building Mozc.

Typical usage:

  % python build_mozc.py gyp
  % python build_mozc.py build_tools -c Release
  % python build_mozc.py build base/base.gyp:base
"""

__author__ = "komatsu"

import glob
import optparse
import os
import re
import shutil
import subprocess
import sys

SRC_DIR = '.'
EXTRA_SRC_DIR = '..'

sys.path.append(SRC_DIR)

from build_tools import mozc_version


def IsWindows():
  """Returns true if the platform is Windows."""
  return os.name == 'nt'


def IsMac():
  """Returns true if the platform is Mac."""
  return os.name == 'posix' and os.uname()[0] == 'Darwin'


def IsLinux():
  """Returns true if the platform is Linux."""
  return os.name == 'posix' and os.uname()[0] == 'Linux'


def GetGeneratorName():
  """Gets the generator name based on the platform."""
  generator = 'make'
  if IsWindows():
    generator = 'msvs'
  elif IsMac():
    generator = 'xcode'
  return generator


def GenerateVersionFile(version_template_path, version_path):
  """Reads the version template file and stores it into version_path.

  This doesn't update the "version_path" if nothing will be changed to
  reduce unnecessary build caused by file timestamp.

  Args:
    version_template_path: a file name which contains the template of version.
    version_path: a file name to be stored the official version.
  """
  version = mozc_version.MozcVersion(version_template_path, expand_daily=True)
  version_definition = version.GetVersionInFormat(
      'MAJOR=@MAJOR@\nMINOR=@MINOR@\nBUILD=@BUILD@\nREVISION=@REVISION@\n')
  old_content = ''
  if os.path.exists(version_path):
    # if the target file already exists, need to check the necessity of update.
    old_content = open(version_path).read()

  if version_definition != old_content:
    open(version_path, 'w').write(version_definition)


def GetVersionFileNames():
  """Gets the (template of version file, version file) pair."""
  template_path = '%s/mozc_version_template.txt' % SRC_DIR
  version_path = '%s/mozc_version.txt' % SRC_DIR
  return (template_path, version_path)


def GetGypFileNames():
  """Gets the list of gyp file names."""
  gyp_file_names = []
  mozc_top_level_names = glob.glob('%s/*' % SRC_DIR)
  # Exclude the gyp directory where we have special gyp files like
  # breakpad.gyp that we should exclude.
  mozc_top_level_names = [x for x in mozc_top_level_names if
                          os.path.basename(x) != 'gyp']
  for name in mozc_top_level_names:
    gyp_file_names.extend(glob.glob(name + '/*.gyp'))
  gyp_file_names.extend(glob.glob('%s/build_tools/*/*.gyp' % SRC_DIR))
  # Include subdirectory of dictionary
  gyp_file_names.append(
      '%s/dictionary/file/dictionary_file.gyp' % SRC_DIR)
  gyp_file_names.append(
      '%s/dictionary/system/system_dictionary.gyp' % SRC_DIR)
  # Include subdirectory of win32 and breakpad for Windows
  if IsWindows():
    gyp_file_names.extend(glob.glob('%s/win32/*/*.gyp' % SRC_DIR))
    gyp_file_names.extend(glob.glob('third_party/breakpad/*.gyp'))
    gyp_file_names.append('third_party/mozc/sandbox/sandbox.gyp')
  elif IsLinux():
    gyp_file_names.extend(glob.glob('%s/unix/*/*.gyp' % SRC_DIR))
    # Add ibus.gyp if ibus is installed.
    # Ubuntu 8.04 (Hardy) does not contain ibus package.
    try:
      RunOrDie(['pkg-config', '--exists', 'ibus-1.0'])
    except RunOrDieError:
      gyp_file_names.remove('%s/unix/ibus/ibus.gyp' % SRC_DIR)
    # Add gui.gyp if Qt libraries are installed.
    try:
      RunOrDie(['pkg-config', '--exists', 'QtCore', 'QtGui'])
    except RunOrDieError:
      gyp_file_names.remove('%s/gui/gui.gyp' % SRC_DIR)
  gyp_file_names.extend(glob.glob('third_party/rx/*.gyp'))
  gyp_file_names.sort()
  return gyp_file_names


def RemoveFile(file_name):
  """Removes the specified file."""
  if not os.path.isfile(file_name):
    return  # Do nothing if not exist.
  if IsWindows():
    # Read-only files cannot be deleted on Windows.
    os.chmod(file_name, 0700)
  print 'Removing file: %s' % file_name
  os.unlink(file_name)


def CopyFile(source, destination):
  """Copies a file to the destination. Remove an old version if needed."""
  if os.path.isfile(destination):  # Remove the old one if exists.
    RemoveFile(destination)
  print 'Copying file to: %s' % destination
  shutil.copy(source, destination)


def RecursiveRemoveDirectory(directory):
  """Removes the specified directory recursively."""
  if os.path.isdir(directory):
    print 'Removing directory: %s' % directory
    if IsWindows():
      # Use RD because shutil.rmtree fails when the directory is readonly.
      RunOrDie(['CMD.exe', '/C', 'RD', '/S', '/Q',
                os.path.normpath(directory)])
    else:
      shutil.rmtree(directory, ignore_errors=True)


def CleanBuildFilesAndDirectories():
  """Cleans build files and directories."""
  # File and directory names to be removed.
  file_names = []
  directory_names = []

  # Collect stuff in the gyp directories.
  gyp_directory_names = [os.path.dirname(f) for f in GetGypFileNames()]
  for gyp_directory_name in gyp_directory_names:
    if IsWindows():
      for pattern in ['*.rules', '*.sln', '*.vcproj']:
        file_names.extend(glob.glob(os.path.join(gyp_directory_name,
                                                 pattern)))
      for build_type in ['Debug', 'Optimize', 'Release']:
        directory_names.append(os.path.join(gyp_directory_name,
                                            build_type))
    elif IsMac():
      directory_names.extend(glob.glob(os.path.join(gyp_directory_name,
                                                    '*.xcodeproj')))
    elif IsLinux():
      file_names.extend(glob.glob(os.path.join(gyp_directory_name,
                                               '*.target.mk')))
  file_names.append('%s/mozc_version.txt' % SRC_DIR)
  file_names.append('third_party/rx/rx.gyp')
  # Collect stuff in the top-level directory.
  directory_names.append('mozc_build_tools')
  if IsMac():
    directory_names.append('xcodebuild')
  elif IsLinux():
    file_names.append('Makefile')
    directory_names.append('out')
  elif IsWindows():
    file_names.append('third_party/breakpad/breakpad.gyp')
    directory_names.append('out_win')
  # Remove files.
  for file_name in file_names:
    RemoveFile(file_name)
  # Remove directories.
  for directory_name in directory_names:
    RecursiveRemoveDirectory(directory_name)


def GetTopLevelSourceDirectoryName():
  """Gets the top level source directory name."""
  if SRC_DIR == '.':
    return SRC_DIR
  script_file_directory_name = os.path.dirname(sys.argv[0])
  num_components = len(SRC_DIR.split('/'))
  dots = ['..'] * num_components
  return os.path.join(script_file_directory_name, '/'.join(dots))


def MoveToTopLevelSourceDirectory():
  """Moves to the build top level directory."""
  os.chdir(GetTopLevelSourceDirectoryName())


def GetGypSvnUrl(deps_file_name):
  """Get the GYP SVN URL from DEPS file."""
  contents = file(deps_file_name).read()
  match = re.search(r'"(http://gyp\.googlecode\.com.*?)"', contents)
  if match:
    return match.group(1)
  else:
    PrintErrorAndExit('GYP URL not found in %s:' % deps_file_name)


def GypMain(deps_file_name):
  options = ParseGypOptions()
  """The main function for the 'gyp' command."""
  # Copy rx.gyp to the third party directory.
  CopyFile('%s/gyp/rx.gyp' % SRC_DIR,
           'third_party/rx/rx.gyp')
  # Copy breakpad.gyp to the third party directory, if necessary.
  if IsWindows():
    CopyFile('%s/gyp/breakpad.gyp' % SRC_DIR,
             'third_party/breakpad/breakpad.gyp')

  # Determine the generator name.
  generator = GetGeneratorName()
  os.environ['GYP_GENERATORS'] = generator
  print 'Build tool: %s' % generator

  # Get and show the list of .gyp file names.
  gyp_file_names = GetGypFileNames()
  print 'GYP files:'
  for file_name in gyp_file_names:
    print '- %s' % file_name
  # We use the one in mozc_build_tools/gyp
  gyp_script = '%s/gyp' % options.gypdir
  # If we don't have a copy of gyp, download it.
  if not os.path.isfile(gyp_script):
    # SVN creates mozc_build_tools directory if it's not present.
    gyp_svn_url = GetGypSvnUrl(deps_file_name)
    RunOrDie(['svn', 'checkout', gyp_svn_url, options.gypdir])
  # Run GYP.
  print 'Running GYP...'
  command_line = [sys.executable, gyp_script,
                  '--no-circular-check',
                  '--depth=.',
                  '--include=%s/gyp/common.gypi' % SRC_DIR]
  if options.onepass:
    command_line.extend(['-D', 'two_pass_build=0'])
  command_line.extend(gyp_file_names)

  if options.branding:
    command_line.extend(['-D', 'branding=%s' % options.branding])
  RunOrDie(command_line)


  # Done!
  print 'Done'


def CleanMain():
  """The main function for the 'clean' command."""
  CleanBuildFilesAndDirectories()


class RunOrDieError(StandardError):
  """The exception class for RunOrDie."""

  def __init__(self, message):
    StandardError.__init__(self, message)


def RunOrDie(argv):
  """Run the command, or die if it failed."""

  # Rest are the target program name and the parameters, but we special
  # case if the target program name ends with '.py'
  if argv[0].endswith('.py'):
    argv.insert(0, sys.executable)  # Inject the python interpreter path.
  # We don't capture stdout and stderr from Popen. The output will just
  # be emitted to a terminal or console.
  print 'Running: ' + ' '.join(argv)
  process = subprocess.Popen(argv)

  if process.wait() != 0:
    raise RunOrDieError('\n'.join(['',
                                   '==========',
                                   ' ERROR: %s' % ' '.join(argv),
                                   '==========']))


def PrintErrorAndExit(error_message):
  """Prints the error message and exists."""
  print error_message
  sys.exit(1)


def ParseGypOptions():
  """Parse command line options for the gyp command."""
  parser = optparse.OptionParser(usage='Usage: %prog gyp [options]')
  parser.add_option('--onepass', '-1', dest='onepass', action='store_true',
                    default=False, help='build mozc in one pass. ' +
                    'Not recommended for Debug build.')
  parser.add_option('--branding', dest='branding', default='Mozc')
  parser.add_option('--gypdir', dest='gypdir', default='mozc_build_tools/gyp')
  (options, unused_args) = parser.parse_args()
  return options


def ParseBuildOptions():
  """Parse command line options for the build command."""
  parser = optparse.OptionParser(usage='Usage: %prog build [options]')
  parser.add_option('--jobs', '-j', dest='jobs', default='4', metavar='N',
                    help='run jobs in parallel')
  parser.add_option('--configuration', '-c', dest='configuration',
                    default='Debug', help='specify the build configuration.')
  parser.add_option('--build_base', dest='build_base',
                    help='specify the base directory of the built binaries.')
  if IsWindows():
    parser.add_option('--platform', '-p', dest='platform',
                      default='Win32',
                      help='specify the target plaform: [Win32|x64]')
  # default Qt dir to support the current build procedure for Debian.
  default_qtdir = '/usr/local/Trolltech/Qt-4.5.2'
  if IsWindows():
    default_qtdir = None
  parser.add_option('--qtdir', dest='qtdir',
                    default=os.getenv('QTDIR', default_qtdir),
                    help='Qt base directory to be used.')

  (options, args) = parser.parse_args()

  targets = args
  if not targets:
    PrintErrorAndExit('No build target is specified.')

  return (options, args)


def ParseTarget(target):
  """Parses the target string."""
  if not ':' in target:
    PrintErrorAndExit('Invalid target: ' + target)
  (gyp_file_name, target_name) = target.split(':')
  return (gyp_file_name, target_name)


def BuildOnLinux(options, targets):
  """Build the targets on Linux."""
  target_names = []
  for target in targets:
    (unused_gyp_file_name, target_name) = ParseTarget(target)
    target_names.append(target_name)

  make_command = os.getenv('BUILD_COMMAND', 'make')
  # flags for building in Chrome OS chroot environment
  envvars = [
      'CFLAGS',
      'CXXFLAGS',
      'CXX',
      'CC',
      'AR',
      'AS',
      'RANLIB',
      'LD',
  ]
  for envvar in envvars:
    if envvar in os.environ:
      os.environ[envvar] = os.getenv(envvar)

  build_args = ['-j%s' % options.jobs, 'BUILDTYPE=%s' % options.configuration]
  if options.build_base:
    build_args.append('builddir_name=%s' % options.build_base)

  RunOrDie([make_command] + build_args + target_names)


def CheckFileOrDie(file_name):
  """Check the file exists or dies if not."""
  if not os.path.isfile(file_name):
    PrintErrorAndExit('No such file: ' + file_name)


def GetRelpath(path, start):
  """Return a relative path to |path| from |start|."""
  # NOTE: Python 2.6 provides os.path.relpath, which has almost the same
  # functionality as this function. Since Python 2.6 is not the internal
  # official version, we reimplement it.
  path_list = os.path.abspath(os.path.normpath(path)).split(os.sep)
  start_list = os.path.abspath(os.path.normpath(start)).split(os.sep)

  common_prefix_count = 0
  for i in range(0, min(len(path_list), len(start_list))):
    if path_list[i] != start_list[i]:
      break
    common_prefix_count += 1

  return os.sep.join(['..'] * (len(start_list) - common_prefix_count) +
                     path_list[common_prefix_count:])


def BuildOnMac(options, targets, original_directory_name):
  """Build the targets on Mac."""
  # For some reason, xcodebuild does not accept absolute path names for
  # the -project parameter. Convert the original_directory_name to a
  # relative path from the build top level directory.
  original_directory_relpath = GetRelpath(original_directory_name, os.getcwd())
  if options.build_base:
    sym_root = options.build_base
  else:
    sym_root = os.path.join(os.getcwd(), 'xcodebuild')
  for target in targets:
    (gyp_file_name, target_name) = ParseTarget(target)
    gyp_file_name = os.path.join(original_directory_relpath, gyp_file_name)
    CheckFileOrDie(gyp_file_name)
    (xcode_base_name, _) = os.path.splitext(gyp_file_name)
    RunOrDie(['xcodebuild',
              '-project', '%s.xcodeproj' % xcode_base_name,
              '-configuration', options.configuration,
              '-target', target_name,
              '-parallelizeTargets',
              'SYMROOT=%s' % sym_root])


def BuildOnWindows(options, targets, original_directory_name):
  """Build the target on Windowsw."""
  # TODO(yukawa): make a python module to set up environment for vcbuild.

  # TODO(yukawa): Locate the directory of the vcbuild.exe as follows.
  #   1. Get the clsid corresponding to 'VisualStudio.VCProjectEngine.8.0'
  #   2. Get the directory of the DLL corresponding to retrieved clsid
  program_files_path = os.getenv('ProgramFiles(x86)',
                                 os.getenv('ProgramFiles'))
  rel_paths = ['Microsoft Visual Studio 8/VC/vcpackages',
               'Microsoft SDKs/Windows/v6.0/VC/Bin']
  abs_vcbuild_dir = ''
  for rel_path in rel_paths:
    search_dir = os.path.join(program_files_path, rel_path)
    if os.path.exists(os.path.join(search_dir, 'vcbuild.exe')):
      abs_vcbuild_dir = os.path.abspath(search_dir)
      break
  CheckFileOrDie(os.path.join(abs_vcbuild_dir, 'vcbuild.exe'))

  if os.getenv('PATH'):
    os.environ['PATH'] = os.pathsep.join([abs_vcbuild_dir, os.getenv('PATH')])
  else:
    os.environ['PATH'] = abs_vcbuild_dir

  rel_paths = ['%s/third_party/platformsdk/v6_1/files/Bin' % EXTRA_SRC_DIR,
               '%s/third_party/code_signing' % EXTRA_SRC_DIR,
               '%s/third_party/vc_80/files/common7/IDE' % EXTRA_SRC_DIR,
               '%s/third_party/vc_80/files/common7/Tools' % EXTRA_SRC_DIR,
               '%s/third_party/vc_80/files/common7/Tools/bin' % EXTRA_SRC_DIR,
               '%s/third_party/wix/v3_0_4220/files' % EXTRA_SRC_DIR]
  rel_paths_x64 = ['%s/third_party/vc_80/files/vc/bin/x86_amd64'
                   % EXTRA_SRC_DIR]
  rel_paths_x86 = ['%s/third_party/vc_80/files/vc/bin' % EXTRA_SRC_DIR]
  if options.platform == 'x64':
    rel_paths += rel_paths_x64
  rel_paths += rel_paths_x86
  abs_paths = [os.path.abspath(path) for path in rel_paths]
  os.environ['PATH'] = os.pathsep.join(abs_paths + [os.getenv('PATH')])

  os.environ['INCLUDE'] = ''
  os.environ['LIB'] = ''
  os.environ['LIBPATH'] = ''

  for target in targets:
    # TODO(yukawa): target name is currently ignored.
    (gyp_file_name, _) = ParseTarget(target)
    gyp_file_name = os.path.join(original_directory_name, gyp_file_name)
    CheckFileOrDie(gyp_file_name)
    (sln_base_name, _) = os.path.splitext(gyp_file_name)
    sln_file_path = os.path.abspath('%s.sln' % sln_base_name)
    # To use different toolsets for vcbuild, we set %PATH%, %INCLUDE%, %LIB%,
    # %LIBPATH% and specify /useenv option here.  See the following article
    # for details.
    # http://blogs.msdn.com/vcblog/archive/2007/12/30/using-different-toolsets-for-vc-build.aspx
    RunOrDie(['vcbuild',
              '/useenv',  # Use %PATH%, %INCLUDE%, %LIB%, %LIBPATH%
              '/M',       # Use concurrent build
              '/time',    # Show build time
              '/platform:%s' % options.platform,
              sln_file_path,
              '%s|%s' % (options.configuration, options.platform)])


def BuildMain(original_directory_name):
  """The main function for the 'build' command."""
  (options, targets) = ParseBuildOptions()

  # Generate a version definition file.
  print 'Generating version definition file...'
  (template_path, version_path) = GetVersionFileNames()
  GenerateVersionFile(template_path, version_path)

  # Set $QTDIR for mozc_tool
  if options.qtdir:
    print 'export $QTDIR = %s' % options.qtdir
    os.environ['QTDIR'] = options.qtdir

  if IsMac():
    BuildOnMac(options, targets, original_directory_name)
  elif IsLinux():
    BuildOnLinux(options, targets)
  elif IsWindows():
    BuildOnWindows(options, targets, original_directory_name)
  else:
    print 'Unsupported platform: ', system


def BuildToolsMain(original_directory_name):
  """The main function for 'build_tools' command."""
  build_tools_dir = os.path.join(GetRelpath(os.getcwd(),
                                            original_directory_name),
                                 '%s/build_tools' % SRC_DIR)
  # build targets in this order
  gyp_files = [
      os.path.join(build_tools_dir, 'primitive_tools', 'primitive_tools.gyp'),
      os.path.join(build_tools_dir, 'build_tools.gyp')
      ]

  for gyp_file in gyp_files:
    (target, _) = os.path.splitext(os.path.basename(gyp_file))
    sys.argv.append('%s:%s' % (gyp_file, target))
    BuildMain(original_directory_name)
    sys.argv.pop()


def ShowHelpAndExit():
  """Shows the help message."""
  print 'Usage: build_mozc.py COMMAND [ARGS]'
  print 'Commands: '
  print '  gyp          Generate project files.'
  print '  build        Build the specified target.'
  print '  build_tools  Build tools used by the build command.'
  print '  clean        Clean all the build files and directories.'
  print ''
  print 'See also the comment in the script for typical usage.'
  sys.exit(1)


def main():
  if len(sys.argv) < 2:
    ShowHelpAndExit()

  # DEPS files should exist in the same directory of the script.
  deps_file_name = os.path.abspath(os.path.join(os.path.dirname(sys.argv[0]),
                                                'DEPS'))
  # Remember the original current directory name.
  original_directory_name = os.getcwd()
  # Move to the top level source directory only once since os.chdir
  # affects functions in os.path and that causes troublesome errors.
  MoveToTopLevelSourceDirectory()

  command = sys.argv[1]
  del(sys.argv[1])  # Delete the command.
  if command == 'build':
    BuildMain(original_directory_name)
  elif command == 'build_tools':
    BuildToolsMain(original_directory_name)
  elif command == 'clean':
    CleanMain()
  elif command == 'gyp':
    GypMain(deps_file_name)
  else:
    print 'Unknown command: ' + command
    ShowHelpAndExit()

if __name__ == '__main__':
  main()
