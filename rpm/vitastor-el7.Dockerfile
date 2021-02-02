# Build packages for CentOS 7 inside a container
# cd ..; podman build -t vitastor-el7 -v `pwd`/build:/root/build -f rpm/vitastor-el7.Dockerfile .
# localedef -i ru_RU -f UTF-8 ru_RU.UTF-8

FROM centos:7

WORKDIR /root

RUN rm -f /etc/yum.repos.d/CentOS-Media.repo
RUN yum -y --enablerepo=extras install centos-release-scl epel-release yum-utils rpm-build
RUN yum -y install https://vitastor.io/rpms/centos/7/vitastor-release-1.0-1.el7.noarch.rpm
RUN yum -y install devtoolset-9-gcc-c++ devtoolset-9-libatomic-devel gperftools-devel qemu-kvm fio rh-nodejs12 jerasure-devel gf-complete-devel
RUN yumdownloader --disablerepo=centos-sclo-rh --source qemu-kvm
RUN yumdownloader --disablerepo=centos-sclo-rh --source fio
RUN rpm --nomd5 -i qemu*.src.rpm
RUN rpm --nomd5 -i fio*.src.rpm
RUN rm -f /etc/yum.repos.d/CentOS-Media.repo
RUN cd ~/rpmbuild/SPECS && yum-builddep -y --enablerepo='*' --disablerepo=centos-sclo-rh --disablerepo=centos-sclo-rh-source --disablerepo=centos-sclo-sclo-testing qemu-kvm.spec
RUN cd ~/rpmbuild/SPECS && yum-builddep -y --enablerepo='*' --disablerepo=centos-sclo-rh --disablerepo=centos-sclo-rh-source --disablerepo=centos-sclo-sclo-testing fio.spec

ADD https://vitastor.io/rpms/liburing-el7/liburing-0.7-2.el7.src.rpm /root

RUN set -e; \
    rpm -i liburing*.src.rpm; \
    cd ~/rpmbuild/SPECS/; \
    . /opt/rh/devtoolset-9/enable; \
    rpmbuild -ba liburing.spec; \
    mkdir -p /root/build/liburing-el7; \
    rm -rf /root/build/liburing-el7/*; \
    cp ~/rpmbuild/RPMS/*/liburing* /root/build/liburing-el7/; \
    cp ~/rpmbuild/SRPMS/liburing* /root/build/liburing-el7/

RUN rpm -i `ls /root/build/liburing-el7/liburing-*.x86_64.rpm | grep -v debug`

ADD . /root/vitastor

RUN set -e; \
    cd /root/vitastor/rpm; \
    sh build-tarball.sh; \
    cp /root/vitastor-0.5.3.el7.tar.gz ~/rpmbuild/SOURCES; \
    cp vitastor-el7.spec ~/rpmbuild/SPECS/vitastor.spec; \
    cd ~/rpmbuild/SPECS/; \
    rpmbuild -ba vitastor.spec; \
    mkdir -p /root/build/vitastor-el7; \
    rm -rf /root/build/vitastor-el7/*; \
    cp ~/rpmbuild/RPMS/*/vitastor* /root/build/vitastor-el7/; \
    cp ~/rpmbuild/SRPMS/vitastor* /root/build/vitastor-el7/
