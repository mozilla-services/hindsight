#!/bin/bash

tag=""
if [[ $1 = "master" ]]; then
    tag="mozilla/hindsight:master"
elif [[ $1 = "dev" ]]; then
    tag="mozilla/hindsight:dev"
else
    exit 1
fi

docker tag mozilla/hindsight:latest $tag
docker login -u "$DOCKER_USER" -p "$DOCKER_PASS"
docker push $tag
