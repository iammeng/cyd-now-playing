FROM python:3.12-slim
# Noto fonts for /text rendering: Thai + Latin (noto-core) and CJK (noto-cjk)
RUN apt-get update && apt-get install -y --no-install-recommends \
    fonts-noto-core fonts-noto-cjk \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY requirements.txt ./
RUN pip install --no-cache-dir -r requirements.txt
COPY app.py auth.py spotify_common.py text_render.py ./
EXPOSE 8080
CMD ["python", "app.py"]
