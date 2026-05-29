ARG IDF_VERSION=v5.5.3
FROM espressif/idf:${IDF_VERSION}

WORKDIR /project

ENV LANG=C.UTF-8
ENV LC_ALL=C.UTF-8

CMD ["bash", "-lc", "scripts/build.sh --build"]
