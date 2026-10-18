FROM python:3.12-slim
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1 RICE_DATA_DIR=/var/lib/rice-cloud
WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY app.py cli.py analysis_worker.py ./
COPY static ./static
RUN useradd --system --create-home --uid 10001 rice && mkdir -p /var/lib/rice-cloud && chown -R rice:rice /app /var/lib/rice-cloud
USER rice
EXPOSE 8000
CMD ["uvicorn", "app:app", "--host", "0.0.0.0", "--port", "8000", "--proxy-headers", "--forwarded-allow-ips", "*"]
