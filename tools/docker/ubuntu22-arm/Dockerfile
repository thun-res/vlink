FROM arm64v8/ubuntu:22.04

#=== args
ARG HOST_USER_UID \
    HOST_USER_GID \
    HOST_PASSWORD

ARG USERNAME=ubuntu \
    DOCKER_NAME=${DOCKER_NAME:-docker} \
    USER_UID=${HOST_USER_UID:-1000} \
    USER_GID=${HOST_USER_GID:-1000} \
    USER_PASSWORD=${HOST_PASSWORD:-123} \
    ROOT_PASSWORD=${HOST_PASSWORD:-123} \
    WORK_DIR=/work

#=== envs
ENV HOME=/home/$USERNAME \
    LANG=en_US.UTF-8 \
    LANGUAGE=en_US.UTF-8 \
    LC_ALL=en_US.UTF-8 \
    TZ=Asia/Shanghai \
    LD_LIBRARY_PATH=/usr/local/lib

#=== timezone
RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone

#=== root
RUN groupadd --gid $USER_GID $USERNAME && useradd --uid $USER_UID --gid $USER_GID -m $USERNAME && \
    echo "root:$ROOT_PASSWORD" | chpasswd && echo "$USERNAME:$USER_PASSWORD" | chpasswd

#=== change apt source to USTC
RUN sed -i 's@http://archive.ubuntu.com/ubuntu/@http://mirrors.ustc.edu.cn/ubuntu/@g' /etc/apt/sources.list && \
    sed -i 's@http://security.ubuntu.com/ubuntu/@http://mirrors.ustc.edu.cn/ubuntu/@g' /etc/apt/sources.list

#=== apt-get
RUN apt-get update
RUN apt-get install -yq --no-install-recommends \
    apt-utils \
    bash-completion \
    sudo \
    tzdata \
    locales \
    gawk \
    wget \
    git \
    git-lfs \
    rpm \
    zsh \
    zsh-syntax-highlighting \
    zsh-autosuggestions \
    subversion \
    diffstat \
    unzip \
    sysstat \
    texinfo \
    build-essential \
    chrpath \
    socat \
    python2 \
    python3 \
    python3-pip \
    python3-distutils \
    python3-pyelftools \
    python3-setuptools \
    xz-utils  \
    cpio \
    autoconf \
    screen \
    tmux \
    iputils-ping \
    iproute2 \
    fluxbox \
    tightvncserver \
    liblz4-tool \
    zstd \
    libzstd-dev \
    net-tools \
    file \
    ca-certificates \
    kmod \
    cmake \
    vim \
    nano \
    tree \
    gdb \
    rsync \
    openssh-server \
    sshpass \
    zlib1g-dev \
    libsqlite3-dev \
    libprotobuf-dev \
    protobuf-compiler \
    flatbuffers-compiler \
    libflatbuffers-dev \
    libprotoc-dev \
    openjdk-17-jdk \
    libasio-dev \
    libtinyxml2-dev \
    libacl1-dev \
    libssl-dev \
    libcgroup-dev \
    libbullet-dev \
    libexpat1-dev \
    lcov \
    libncurses5 \
    libtinfo5 \
    device-tree-compiler \
    doxygen \
    graphviz \
    android-tools-adb \
    fastboot \
    ninja-build \
    apache2 \
    samba \
    samba-common \
    dos2unix \
    zip \
    less \
    sqlite3 \
    postgresql \
    curl \
    automake \
    pkgconf \
    tclsh \
    xmlstarlet \
    libarchive-dev \
    lz4 \
    libselinux1-dev \
    fakeroot \
    fakechroot \
    ccache \
    mtd-utils \
    dosfstools \
    libiberty-dev \
    binutils-dev \
    util-linux \
    uuid-dev \
    libpam0g-dev \
    bison \
    flex \
    swig \
    u-boot-tools \
    udisks2 \
    libpng-dev \
    libjpeg-dev \
    gnupg2 \
    lsb-release \
    libusb-dev \
    zlib1g \
    tcpdump \
    tcpreplay \
    libavcodec-dev \
    libavdevice-dev \
    libavfilter-dev \
    libavformat-dev \
    libavutil-dev \
    libpostproc-dev \
    libswresample-dev \
    libswscale-dev \
    libxml2-dev \
    libfontconfig1-dev \
    libfreetype6-dev \
    libcapture-tiny-perl \
    libdatetime-perl \
    libdatetime-format-dateparse-perl \
    libva-dev \
    libvdpau-dev \
    libx11-dev \
    libx11-xcb-dev \
    libfontenc-dev \
    libice-dev \
    libsm-dev \
    libxau-dev \
    libxaw7-dev \
    libxcomposite-dev \
    libxcursor-dev \
    libxdamage-dev \
    libxfixes-dev \
    libxi-dev \
    libxinerama-dev \
    libxkbfile-dev \
    libxmuu-dev \
    libxrandr-dev \
    libxrender-dev \
    libxres-dev \
    libxss-dev \
    libxtst-dev \
    libxv-dev \
    libxxf86vm-dev \
    libxcomposite-dev \
    libxcursor-dev \
    libxdamage-dev \
    libxfixes-dev \
    libxi-dev \
    libxinerama-dev \
    libxkbfile-dev \
    libxmuu-dev \
    libxrandr-dev \
    libxrender-dev \
    libxres-dev \
    libxss-dev \
    libxtst-dev \
    libxv-dev \
    libxxf86vm-dev \
    libxcb-util-dev \
    libxcb-glx0-dev \
    libxcb-render0-dev \
    libxcb-render-util0-dev \
    libxcb-xkb-dev \
    libxcb-icccm4-dev \
    libxcb-image0-dev \
    libxcb-keysyms1-dev \
    libxcb-randr0-dev \
    libxcb-shape0-dev \
    libxcb-sync-dev \
    libxcb-xfixes0-dev \
    libxcb-xinerama0-dev \
    libxcb-dri3-dev \
    libxcb-cursor-dev \
    libxcb-dri2-0-dev \
    libxcb-dri3-dev \
    libxcb-present-dev \
    libxcb-composite0-dev \
    libxcb-ewmh-dev \
    libxcb-res0-dev \
    libxkbcommon-dev \
    libgl1-mesa-dev \
    libncurses5-dev \
    qtbase5-dev

#=== user
RUN echo "\n$USERNAME ALL=(ALL) ALL\n" >> /etc/sudoers && \
    mkdir -p $WORK_DIR && chown $USER_UID:$USER_UID $WORK_DIR && \
    update-ca-certificates && \
    locale-gen en_US en_US.UTF-8 && update-locale

#=== python
RUN python3 -m pip config --no-cache-dir --global set global.index-url https://pypi.mirrors.ustc.edu.cn/simple/ && \
    python3 -m pip config --no-cache-dir --global set global.trusted-host pypi.mirrors.ustc.edu.cn
RUN python3 -m pip install --no-cache-dir -U \
    pip \
    setuptools \
    wheel
RUN python3 -m pip install --no-cache-dir \
    vcs2l \
    conan \
    jinja2
RUN python3 -m pip install --no-cache-dir \
    "cmake==3.30.9"

#=== bash
ARG DOCKER_VERSION=${DOCKER_VERSION:-1.0.0}
RUN echo "echo '\033[35mYou are in $DOCKER_NAME(${DOCKER_VERSION}), enjoy it!\033[0m'" >> /etc/bash.bashrc

#=== zsh
COPY zsh_custom /etc/zsh/zsh_custom
RUN mkdir -p /home/$USERNAME && \
    touch /home/$USERNAME/.zshrc && \
    chown $USER_UID:$USER_UID /home/$USERNAME/.zshrc && \
    echo "### zsh custom ###" >> /etc/zsh/zshrc && \
    echo "[ -f /etc/zsh/zsh_custom ] && . /etc/zsh/zsh_custom" >> /etc/zsh/zshrc && \
    echo "echo '\033[35mYou are in $DOCKER_NAME(${DOCKER_VERSION}), enjoy it!\033[0m'" >> /etc/zsh/zshrc


#=== clean
RUN apt-get autoremove && apt-get autoclean && apt-get clean && \
    rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/* /home/root/* /var/log/*.log /var/log/*/*.log /root/.cache

#=== start
USER $USERNAME
WORKDIR $WORK_DIR
CMD ["zsh"]
