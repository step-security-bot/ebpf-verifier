FROM ubuntu:20.04@sha256:bb1c41682308d7040f74d103022816d41c50d7b0c89e9d706a74b4e548636e54

ENV DEBIAN_FRONTEND=noninteractive
RUN apt update
RUN apt -yq --no-install-suggests --no-install-recommends install build-essential cmake \
    libboost-dev libboost-filesystem-dev libboost-program-options-dev libyaml-cpp-dev
WORKDIR /verifier
COPY . /verifier/
RUN mkdir build
WORKDIR /verifier/build
RUN cmake .. -DCMAKE_BUILD_TYPE=Release
RUN make -j $(nproc)
WORKDIR /verifier
ENTRYPOINT ["./check"]
