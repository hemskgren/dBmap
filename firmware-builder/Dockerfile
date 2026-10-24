FROM espressif/idf:v5.4.4@sha256:9ce51d4ace496845678065dc86f8e55fb9c25fc9f01f45597042afa7ed546926

WORKDIR /app
COPY server.py /app/server.py
COPY entrypoint.sh /entrypoint.sh
RUN chmod 755 /entrypoint.sh

ENV PYTHONUNBUFFERED=1
EXPOSE 8443
ENTRYPOINT ["/entrypoint.sh"]
CMD ["python", "/app/server.py"]
