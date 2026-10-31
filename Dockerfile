FROM gcc:14-bookworm AS build
WORKDIR /build
COPY src/ src/
COPY third_party/ third_party/
RUN g++ -std=c++17 -O2 -pthread src/main.cpp -Ithird_party -o lab_scheduler

FROM gcc:14-bookworm AS runtime
RUN useradd --system --create-home --shell /usr/sbin/nologin scheduler \
    && mkdir -p /app/data \
    && chown -R scheduler:scheduler /app
WORKDIR /app
COPY --from=build /build/lab_scheduler ./lab_scheduler
COPY --chown=scheduler:scheduler web/ ./web/
USER scheduler
ENV PORT=8080
ENV LAB_HOST=0.0.0.0
EXPOSE 8080
ENTRYPOINT ["./lab_scheduler"]
