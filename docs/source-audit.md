# Source audit notes

Audit date: 2026-07-10.

## Shared design discussion

The shared ChatGPT discussion consistently recommended a ROS-free C++ stereo
engine, a very small ROS publisher boundary, preallocated latest-frame buffers,
and no dense point cloud by default. It considered host-native, one-process, and
containerized deployment; those have been reduced to the three explicit choices
in `architecture-options.md`.

## Read-only Nuvo files inspected

- `ROS_package/nuvo_perception/launch/front_stereo.launch.py`
- `ROS_package/nuvo_perception/launch/stereo_depth.launch.py`
- `ROS_package/nuvo_perception/config/front_stereo/front_left.yaml`
- `ROS_package/nuvo_perception/config/front_stereo/front_right.yaml`
- `ROS_package/nuvo_stereo_sync/*`
- `ROS_package/nuvo_depth_obstacles/*`
- `ROS_package/robot_bringup/launch/bringup.launch.py`
- `ROS_package/robot_v1_description/urdf/sensors.xacro`
- `deployment/robot/docker-compose.robot.yml`
- `deployment/robot/.env.example`
- `deployment/robot/tools/camera_depth*_topics.txt`

The Nuvo workspace already contained unrelated modified and untracked files.
Nothing in that workspace was edited, staged, or committed.
