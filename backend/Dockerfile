FROM python:3.11-slim

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY . .

# Least privilege: the app never needs root once dependencies are installed.
RUN useradd --system --no-create-home --uid 1501 appuser && \
    chown -R appuser:appuser /app
USER appuser

EXPOSE 5000

# Single worker keeps flask-limiter's in-memory rate limiting consistent
# (separate worker processes would each track their own counters); threads
# still let one slow request (e.g. a stalled InfluxDB write) not block a
# concurrent one. Traffic here is one device plus occasional dashboard
# reads, well within what a single worker handles.
CMD ["gunicorn", "--bind", "0.0.0.0:5000", "--workers", "1", "--threads", "4", "main-flask:app"]
