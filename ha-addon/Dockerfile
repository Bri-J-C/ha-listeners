ARG BUILD_FROM
FROM ${BUILD_FROM}

# Install dependencies
RUN apk add --no-cache \
    python3 \
    py3-pip \
    opus \
    opus-dev \
    ffmpeg \
    gcc \
    musl-dev \
    python3-dev

# Install Python packages
COPY requirements.txt /
RUN pip3 install --no-cache-dir -r /requirements.txt

# Copy application
COPY intercom_hub.py /
COPY run.sh /

RUN chmod a+x /run.sh

CMD [ "/run.sh" ]
