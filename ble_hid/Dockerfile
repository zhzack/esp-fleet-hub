ARG ESP_IDF_IMAGE=espressif/idf:release-v5.4
FROM ${ESP_IDF_IMAGE}

WORKDIR /project

ENV HOME=/tmp
ENV PATH=/opt/esp/idf/tools:${PATH}

CMD ["idf.py", "build"]
