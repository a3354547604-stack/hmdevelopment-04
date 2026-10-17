# 后端镜像：python:3.12-slim（与 docker pull 可用性无关的本机缓存镜像见 README）
# 目的：镜像内自带 pytest/httpx，`docker compose run --rm api pytest` 与运行期共用同一套依赖。
FROM python:3.12-slim

ARG PIP_INDEX_URL=https://pypi.org/simple

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    TZ=UTC \
    PIP_INDEX_URL=${PIP_INDEX_URL} \
    PIP_DISABLE_PIP_VERSION_CHECK=1

WORKDIR /srv

# 先装依赖再加代码，代码改动不会让依赖层失效
COPY requirements.txt requirements-dev.txt ./
RUN pip install --no-cache-dir -r requirements-dev.txt

COPY . .

EXPOSE 3000

# 健康检查用标准库，避免额外装 curl
HEALTHCHECK --interval=15s --timeout=5s --start-period=20s --retries=5 \
  CMD python -c "import urllib.request,sys; sys.exit(0 if urllib.request.urlopen('http://127.0.0.1:3000/api/health', timeout=4).status==200 else 1)"

CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "3000"]
