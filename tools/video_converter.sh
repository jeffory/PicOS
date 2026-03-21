#!/bin/bash

FILE="$1"
CROP_VIDEO=false
VIDEO_QUALITY=8
AUDIO_MUTED=false

# Check if ffmpeg is installed
if ! command -v ffmpeg &> /dev/null; then
    echo "ffmpeg is not installed. Please install it and try again."
    exit 1
fi

if [ -z "$FILE" ]; then
    echo "Usage: $0 <video_file_or_url>"
    exit 1
fi

while [[ "$1" == -* ]]; do
    case "$1" in
        --crop)
            CROP_VIDEO=true
            shift
            ;;
        --quality)
            shift
            VIDEO_QUALITY="$1"
            shift
            ;;
        --mute)
            AUDIO_MUTED=true
            shift
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

if [ "$AUDIO_MUTED" = true ]; then
    AUDIO_OPTS="-an"
else
    AUDIO_OPTS="-acodec libmp3lame -ab 128k -ar 22050"
fi


# If input starts with http, use yt-dlp to download
if [[ "$FILE" =~ ^http ]]; then
    # Check if yt-dlp is installed for downloading videos from URLs
    if ! command -v yt-dlp &> /dev/null; then
        echo "yt-dlp is not installed. Please install it to download videos from URLs."
        exit 1
    fi

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
        $AUDIO_OPTS \
        "$OUTPUT_DIR/${BASENAME}.avi"
else
    ffmpeg -i "$FILE" \
        -vf "fps=25,scale=320:-1" \
        -vcodec mjpeg \
        -q:v $VIDEO_QUALITY \
        -huffman optimal \
        $AUDIO_OPTS \
        "$OUTPUT_DIR/${BASENAME}.avi"
fi
