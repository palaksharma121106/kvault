FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends g++ \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY kvault.cpp router.cpp hashring.h ./

RUN g++ -std=c++17 -O2 -pthread -o kvault kvault.cpp \
 && g++ -std=c++17 -O2 -pthread -o router router.cpp

CMD ["./kvault", "server", "6379"]