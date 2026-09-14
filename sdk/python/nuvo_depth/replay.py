from __future__ import annotations

import argparse

from .recording import RecordingReader


def main() -> None:
    parser = argparse.ArgumentParser(description="Inspect a Nuvo recording")
    parser.add_argument("recording")
    args = parser.parse_args()
    with RecordingReader(args.recording) as recording:
        for frame in recording:
            print(frame.capture_epoch, frame.capture_sequence,
                  ",".join(frame.products))


if __name__ == "__main__": main()
