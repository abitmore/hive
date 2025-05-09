#! /bin/bash

function build_image_name() {
  local tag=${1:?"Missing tag"}
  local registry=${2:?"Missing registry"}
  local image=${3:-}
  # If image is not empty then it needs to be preceeded by a slash 
  echo "${registry}${image:+/$image}:${tag}"
}

function build_image_registry_path() {
  local tag=${1:?"Missing tag"}
  local registry=${2:?"Missing registry"}
  local image=${3:-}
  # If image is not empty then it needs to be preceeded by a slash 
  echo "${registry}${image:+/$image}"
}

function docker_image_exists() {
  local imgname=${1:?"Missing image name"}
  local  __resultvar=${2:?"Missing required retval variable name"}
  local look_to_registry=${3:-1}

  local result=0

  local old_set_e=0

  [[ $- = *e* ]] && old_set_e=1
  set +e

  echo "Looking for: $imgname"
  
  if [ "${look_to_registry}" -eq "1" ];
  then
    docker manifest inspect "$imgname" >/dev/null 2>&1
    result=$?
    echo "docker manifest inspect returned $result"
  else
    docker image inspect --format '{{.Id}}' "$imgname" >/dev/null 2>&1
    result=$?
    echo "docker image inspect returned $result"
  fi

  ((old_set_e)) && set -e


  local EXISTS=0

  if [ "$result" -eq "0" ];
  then
    EXISTS=1
  fi

  if [[ "$__resultvar" ]]; then
      eval "$__resultvar"="'$EXISTS'"
  else
      echo "$EXISTS"
  fi
}

extract_files_from_image() {
  IMAGE_TAGGED_NAME=${1:?"Missing image name"}
  EXPORT_PATH=${2:?"Missing export target directory"}
  _TST_ANY_FILE=${3:?"Missing file(s) to be exported"}
  shift
  shift 

  FILES=("$@")

  echo "Attempting to export file(s): ${FILES[*]} from image: ${IMAGE_TAGGED_NAME} into directory: ${EXPORT_PATH}"

  export DOCKER_BUILDKIT=1

  docker build -o "${EXPORT_PATH}" - << EOF
    FROM scratch
    COPY --from=${IMAGE_TAGGED_NAME} "${FILES[@]}" /
EOF

}

