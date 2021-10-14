local apt_get_quiet = 'apt-get -o=Dpkg::Use-Pty=0 -q ';

local debian_pipeline(name, image, arch='amd64', deps='g++ libsodium-dev libzmq3-dev', cmake_extra='', build_type='Release', extra_cmds=[], allow_fail=false) = {
    kind: 'pipeline',
    type: 'docker',
    name: name,
    platform: { arch: arch },
    environment: { CLICOLOR_FORCE: '1' },  // Lets color through ninja (1.9+)
    steps: [
        {
            name: 'build',
            image: image,
            [if allow_fail then 'failure']: 'ignore',
            commands: [
                'echo "Building on ${DRONE_STAGE_MACHINE}"',
                'echo "man-db man-db/auto-update boolean false" | debconf-set-selections',
                apt_get_quiet + 'update',
                apt_get_quiet + 'install -y eatmydata',
                'eatmydata ' + apt_get_quiet + 'dist-upgrade -y',
                'eatmydata ' + apt_get_quiet + 'install -y cmake git ninja-build pkg-config ccache ' + deps,
                'git submodule update --init --recursive',
                'mkdir build',
                'cd build',
                'cmake .. -G Ninja -DCMAKE_CXX_FLAGS=-fdiagnostics-color=always -DCMAKE_BUILD_TYPE=' + build_type + ' -DCMAKE_CXX_COMPILER_LAUNCHER=ccache ' + cmake_extra,
                'ninja -v',
                './tests/tests --use-colour yes',
            ] + extra_cmds,
        },
    ],
};

local clang(version) = debian_pipeline(
    'Debian sid/clang-' + version + ' (amd64)',
    'debian:sid',
    deps='clang-' + version + ' libsodium-dev libzmq3-dev',
    cmake_extra='-DCMAKE_C_COMPILER=clang-' + version + ' -DCMAKE_CXX_COMPILER=clang++-' + version + ' '
);

local full_llvm(version) = debian_pipeline(
    'Debian sid/llvm-' + version + ' (amd64)',
    'debian:sid',
    deps='clang-' + version + ' lld-' + version + ' libc++-' + version + '-dev libc++abi-' + version + '-dev libsodium-dev libzmq3-dev',
    cmake_extra='-DCMAKE_C_COMPILER=clang-' + version +
                ' -DCMAKE_CXX_COMPILER=clang++-' + version +
                ' -DCMAKE_CXX_FLAGS=-stdlib=libc++ ' +
                std.join(' ', [
                    '-DCMAKE_' + type + '_LINKER_FLAGS=-fuse-ld=lld-' + version
                    for type in ['EXE', 'MODULE', 'SHARED', 'STATIC']
                ])
);


[
    debian_pipeline('Ubuntu focal (amd64)', 'ubuntu:focal'),
    debian_pipeline('Ubuntu bionic (amd64)',
                    'ubuntu:bionic',
                    deps='libsodium-dev g++-8',
                    cmake_extra='-DCMAKE_C_COMPILER=gcc-8 -DCMAKE_CXX_COMPILER=g++-8'),
    debian_pipeline('Debian sid (amd64)', 'debian:sid'),
    debian_pipeline('Debian sid/Debug (amd64)', 'debian:sid', build_type='Debug'),
    clang(13),
    full_llvm(13),
    debian_pipeline('Debian buster (amd64)', 'debian:buster'),
    debian_pipeline('Debian buster (i386)', 'i386/debian:buster'),
    debian_pipeline('Ubuntu bionic (ARM64)',
                    'ubuntu:bionic',
                    arch='arm64',
                    deps='libsodium-dev g++-8',
                    cmake_extra='-DCMAKE_C_COMPILER=gcc-8 -DCMAKE_CXX_COMPILER=g++-8'),
    debian_pipeline('Debian sid (ARM64)', 'debian:sid', arch='arm64'),
    debian_pipeline('Debian buster (armhf)', 'arm32v7/debian:buster', arch='arm64'),
    {
        kind: 'pipeline',
        type: 'exec',
        name: 'macOS (Catalina w/macports)',
        platform: { os: 'darwin', arch: 'amd64' },
        environment: { CLICOLOR_FORCE: '1' },  // Lets color through ninja (1.9+)
        steps: [
            {
                name: 'build',
                commands: [
                    'git submodule update --init --recursive',
                    'mkdir build',
                    'cd build',
                    'cmake .. -G Ninja -DCMAKE_CXX_FLAGS=-fcolor-diagnostics -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER_LAUNCHER=ccache',
                    'ninja -v',
                    './tests/tests --use-colour yes',
                ],
            },
        ],
    },
]
