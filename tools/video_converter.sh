#!/bin/bash

FILE="$1"
CROP_VIDEO=false
VIDEO_QUALITY=8

if [ -z "$FILE" ]; then
    echo "Usage: $0 <video_file_or_url>"
    exit 1
fi

# If input starts with http, use yt-dlp to download
if [[ "$FILE" =~ ^http ]]; then
    TMPFILE=$(mktemp --suffix=.mp4)
    echo "Downloading $FILE with yt-dlp..."
    yt-dlp -f best -o "$TMPFILE" "$FILE"
    if [ $? -ne 0 ]; then
        echo "yt-dlp failed to download video."
        exit 1
    fi
    FILE="$TMPFILE"
fi

OUTPUT_DIR="$HOME/Videos/PicOS/"
if [ ! -d "$OUTPUT_DIR" ]; then
    echo "Output directory $OUTPUT_DIR does not exist."
    exit 1
fi

FILENAME=$(basename "$FILE")
EXTENSION="${FILENAME##*.}"
BASENAME="${FILENAME%.*}"

echo "Converting $FILE to AVI format for PicOS video player..."


if [ "$CROP_VIDEO" = true ]; then
    ffmpeg -i "$FILE" \
        -vf "fps=25,scale=320:320:force_original_aspect_ratio=increase,crop=320:320" \
        -vcodec mjpeg \
        -q:v $VIDEO_QUALITY \
        -huffman optimal \
        -an \
        "$OUTPUT_DIR/${BASENAME}.avi"
else
    ffmpeg -i "$FILE" \
        -vf "fps=25,scale=320:-1" \
        -vcodec mjpeg \
        -q:v $VIDEO_QUALITY \
        -huffman optimal \
        -an \
        "$OUTPUT_DIR/${BASENAME}.avi"
fi
