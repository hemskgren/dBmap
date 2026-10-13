FROM espressif/idf:v5.4.2@sha256:4317595a911886e04cbc6d731315d5fc2918e510b6733fc7d03381ab2d270082

WORKDIR /app
COPY server.py /app/server.py

EXPOSE 8090
CMD ["python", "/app/server.py"]
