local distro = "sid";

local apt_get_quiet = 'apt-get -o=Dpkg::Use-Pty=0 -q';

local repo_suffix = ''; // can be /beta or /staging for non-primary repo deps

local deb_pipeline(name, image, buildarch='amd64', debarch='amd64', jobs=6) = {
    kind: 'pipeline',
    type: 'docker',
    name: name,
    platform: { arch: buildarch },
    steps: [
        {
            name: 'build',
            image: image,
            environment: { SSH_KEY: { from_secret: "SSH_KEY" } },
            commands: [
                'echo "man-db man-db/auto-update boolean false" | debconf-set-selections',
                'cp debian/deb.loki.network.gpg /etc/apt/trusted.gpg.d/deb.loki.network.gpg',
                'echo deb https://deb.loki.network' + repo_suffix + ' ' + distro + ' main >/etc/apt/sources.list.d/loki.list',
                apt_get_quiet + ' update',
                apt_get_quiet + ' install -y eatmydata',
                'eatmydata ' + apt_get_quiet + ' dist-upgrade -y',
                'eatmydata ' + apt_get_quiet + ' install --no-install-recommends -y git-buildpackage devscripts equivs g++ ccache openssh-client',
                'eatmydata dpkg-reconfigure ccache',
                'cd debian',
                'eatmydata mk-build-deps -i -r --tool="' + apt_get_quiet + ' -o Debug::pkgProblemResolver=yes --no-install-recommends -y" control',
                'cd ..',
                'eatmydata gbp buildpackage --git-no-pbuilder --git-builder=\'debuild --prepend-path=/usr/lib/ccache --preserve-envvar=CCACHE_*\' --git-upstream-tag=HEAD -us -uc -j' + jobs,
                './debian/ci-upload.sh ' + distro + ' ' + debarch,
            ],
        }
    ]
};

[
    deb_pipeline("Debian sid (amd64)", "debian:sid"),
    deb_pipeline("Debian sid (i386)", "i386/debian:sid", buildarch='amd64', debarch='i386'),
    deb_pipeline("Debian sid (arm64)", "arm64v8/debian:sid", buildarch='arm64', debarch="arm64", jobs=4),
    deb_pipeline("Debian sid (armhf)", "arm32v7/debian:sid", buildarch='arm64', debarch="armhf", jobs=4),
]
