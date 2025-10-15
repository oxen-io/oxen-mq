local docker_base = 'registry.oxen.rocks/';

local default_deps_nocxx = ['libsodium-dev', 'libzmq3-dev', 'liboxenc-dev'];

local submodule_commands = ['git fetch --tags', 'git submodule update --init --recursive --depth=1'];

local submodules = {
  name: 'submodules',
  image: 'drone/git',
  commands: submodule_commands,
};

local apt_get_quiet = 'apt-get -o=Dpkg::Use-Pty=0 -q ';


local generic_build(build_type, cmake_extra, werror=false, tests=true)
      = [
          'mkdir build',
          'cd build',
          'cmake .. -G Ninja -DCMAKE_COLOR_DIAGNOSTICS=ON -DCMAKE_BUILD_TYPE=' + build_type +
          ' -DWARNINGS_AS_ERRORS=' + (if werror then 'ON' else 'OFF') +
          ' -DOXENMQ_BUILD_TESTS=' + (if tests then 'ON' else 'OFF') +
          ' ' + cmake_extra,
          'ninja -v',
          'cd ..',
        ]
        + (if tests then [
             'cd build',
             './tests/tests --colour-mode ansi',
             'cd ..',
           ] else []);


local debian_pipeline(name,
                      image,
                      arch='amd64',
                      deps=['g++'] + default_deps_nocxx,
                      cmake_extra='',
                      build_type='Release',
                      extra_cmds=[],
                      werror=false,
                      distro='$$(lsb_release -sc)',
                      allow_fail=false) = {
  kind: 'pipeline',
  type: 'docker',
  name: name,
  platform: { arch: arch },
  environment: { CLICOLOR_FORCE: '1' },  // Lets color through ninja (1.9+)
  steps: [
    submodules,
    {
      name: 'build',
      image: image,
      pull: 'always',
      [if allow_fail then 'failure']: 'ignore',
      commands: [
                  'echo "Building on ${DRONE_STAGE_MACHINE}"',
                  'echo "man-db man-db/auto-update boolean false" | debconf-set-selections',
                  apt_get_quiet + 'update',
                  apt_get_quiet + 'install -y eatmydata',
                  'eatmydata ' + apt_get_quiet + ' install --no-install-recommends -y lsb-release',
                  'cp contrib/deb.oxen.io.gpg /etc/apt/trusted.gpg.d',
                  'echo deb http://deb.oxen.io ' + distro + ' main >/etc/apt/sources.list.d/oxen.list',
                  'eatmydata ' + apt_get_quiet + ' update',
                  'eatmydata ' + apt_get_quiet + 'dist-upgrade -y',
                  'eatmydata ' + apt_get_quiet + 'install -y cmake git ninja-build pkg-config ccache ' + std.join(' ', deps),
                ]
                + generic_build(build_type, cmake_extra, werror=werror)
                + extra_cmds,
    },
  ],
};

local clang(version) = debian_pipeline(
  'Debian sid/clang-' + version + ' (amd64)',
  docker_base + 'debian-sid-clang',
  distro='sid',
  deps=['clang-' + version, 'clang-tools-' + version] + default_deps_nocxx,
  cmake_extra='-DCMAKE_C_COMPILER=clang-' + version + ' -DCMAKE_CXX_COMPILER=clang++-' + version + ' '
);

local full_llvm(version) = debian_pipeline(
  'Debian sid/llvm-' + version + ' (amd64)',
  docker_base + 'debian-sid-clang',
  distro='sid',
  deps=['clang-' + version, 'clang-tools-' + version, 'lld-' + version, 'libc++-' + version + '-dev', 'libc++abi-' + version + '-dev']
       + default_deps_nocxx,
  cmake_extra='-DCMAKE_C_COMPILER=clang-' + version +
              ' -DCMAKE_CXX_COMPILER=clang++-' + version +
              ' -DCMAKE_CXX_FLAGS=-stdlib=libc++ ' +
              std.join(' ', [
                '-DCMAKE_' + type + '_LINKER_FLAGS=-fuse-ld=lld-' + version
                for type in ['EXE', 'MODULE', 'SHARED', 'STATIC']
              ])
);

local mac_builder(name,
                  build_type='Release',
                  arch='amd64',
                  cmake_extra='-DCMAKE_CXX_COMPILER_LAUNCHER=ccache ',
                  extra_cmds=[],
                  tests=true) = {
  kind: 'pipeline',
  type: 'exec',
  name: name,
  platform: { os: 'darwin', arch: arch },
  steps: [
    { name: 'submodules', commands: submodule_commands },
    {
      name: 'build',
      environment: { SSH_KEY: { from_secret: 'SSH_KEY' } },
      commands: [
                  'echo "Building on ${DRONE_STAGE_MACHINE}"',
                  // If you don't do this then the C compiler doesn't have an include path containing
                  // basic system headers.  WTF apple:
                  'export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"',
                  'ulimit -n 1024',  // Because macOS has a stupid tiny default ulimit
                ]
                + generic_build(build_type, cmake_extra)
                + extra_cmds,
    },
  ],
};

[
  debian_pipeline('Debian sid (amd64)', docker_base + 'debian-sid', distro='sid'),
  debian_pipeline('Debian sid/Debug (amd64)', docker_base + 'debian-sid', build_type='Debug', distro='sid'),
  clang(17),
  full_llvm(19),
  debian_pipeline('Debian sid (ARM64)', docker_base + 'debian-sid', arch='arm64', distro='sid'),
  debian_pipeline('Debian stable (i386)', docker_base + 'debian-stable/i386'),
  debian_pipeline('Debian stable (armhf)', docker_base + 'debian-stable/arm32v7', arch='arm64'),
  debian_pipeline('Debian bullseye (amd64)', docker_base + 'debian-bullseye'),
  debian_pipeline('Debian bullseye (armhf)', docker_base + 'debian-bullseye/arm32v7', arch='arm64'),
  debian_pipeline('Ubuntu focal (amd64)',
                  docker_base + 'ubuntu-focal',
                  deps=default_deps_nocxx + ['g++-10'],
                  cmake_extra='-DCMAKE_C_COMPILER=gcc-10 -DCMAKE_CXX_COMPILER=g++-10'),
  debian_pipeline('Ubuntu noble (amd64)', docker_base + 'ubuntu-noble'),
  mac_builder('MacOS (amd64) Release', build_type='Release', arch='amd64'),
  mac_builder('MacOS (amd64) Debug', build_type='Debug', arch='amd64'),
  mac_builder('MacOS (arm64) Release', build_type='Release', arch='arm64'),
  mac_builder('MacOS (arm64) Debug', build_type='Debug', arch='arm64'),
]
