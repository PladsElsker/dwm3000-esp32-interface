"""Planar UWB + 9-DoF IMU error-state Kalman filter.

Dependencies
------------
    pip install numpy

Coordinate and transform conventions
------------------------------------
* World frame: right-handed, X/Y lie in the floor plane, +Z is up.
* ``T_body_imu`` maps IMU sensor coordinates into the rigid-body frame.
* ``T_body_tag`` maps the UWB tag frame into the rigid-body frame.
* Each ``T_world_anchors[id]`` maps that anchor's local frame into world.
* ``T_world_marker`` maps the fixed marker frame into world.
* ``update_marker`` receives ``T_body_marker``: marker coordinates mapped
  into the body frame. Compose the camera extrinsic with the camera-relative
  detection externally before calling this method.
* A 4x4 transform is a conventional homogeneous transform:
      [ R  t ]
      [ 0  1 ]
* Quaternions are stored internally as [w, x, y, z] and rotate body vectors
  into world coordinates.
* Accelerometer input is specific force in m/s^2, gyro input is rad/s, and
  magnetometer input is calibrated microtesla (uT). Range input is metres.
* Timestamps are seconds on one monotonic clock shared by the IMU and UWB.

The filter estimates a full 3D inertial state but applies a soft planar
constraint to body-origin height and vertical velocity. Its public position
is the body-origin XY position; ``tag_position_xy`` is also returned.

This is a practical starting point, not a replacement for sensor calibration,
clock synchronization, antenna-delay calibration, or site-specific validation.
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass
from typing import Deque, Dict, Mapping, Optional, Sequence, Tuple, Union

import numpy as np

ArrayLike = Union[Sequence[float], np.ndarray]


def _vec(value: ArrayLike, n: int, name: str) -> np.ndarray:
    arr = np.asarray(value, dtype=float).reshape(-1)
    if arr.shape != (n,) or not np.all(np.isfinite(arr)):
        raise ValueError(f"{name} must contain {n} finite values")
    return arr.copy()


def _transform(value: Optional[ArrayLike], name: str) -> np.ndarray:
    if value is None:
        return np.eye(4, dtype=float)
    T = np.asarray(value, dtype=float)
    if T.shape != (4, 4) or not np.all(np.isfinite(T)):
        raise ValueError(f"{name} must be a finite 4x4 matrix")
    if not np.allclose(T[3], [0.0, 0.0, 0.0, 1.0], atol=1e-7):
        raise ValueError(f"{name} has an invalid homogeneous bottom row")
    R = T[:3, :3]
    if not np.allclose(R.T @ R, np.eye(3), atol=1e-5):
        raise ValueError(f"{name} rotation must be orthonormal")
    if not np.isclose(np.linalg.det(R), 1.0, atol=1e-5):
        raise ValueError(f"{name} rotation must have determinant +1")
    return T.copy()


def _skew(v: np.ndarray) -> np.ndarray:
    x, y, z = v
    return np.array([[0.0, -z, y], [z, 0.0, -x], [-y, x, 0.0]])


def _normalize(v: np.ndarray, eps: float = 1e-12) -> Optional[np.ndarray]:
    n = float(np.linalg.norm(v))
    if not np.isfinite(n) or n < eps:
        return None
    return v / n


def _quat_normalize(q: np.ndarray) -> np.ndarray:
    n = float(np.linalg.norm(q))
    if n < 1e-12 or not np.isfinite(n):
        raise ValueError("Invalid quaternion")
    return q / n


def _quat_mul(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    aw, ax, ay, az = a
    bw, bx, by, bz = b
    return np.array([
        aw * bw - ax * bx - ay * by - az * bz,
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
    ])


def _quat_from_rotvec(phi: np.ndarray) -> np.ndarray:
    angle = float(np.linalg.norm(phi))
    if angle < 1e-10:
        # First-order expansion, followed by normalization.
        return _quat_normalize(np.r_[1.0, 0.5 * phi])
    half = 0.5 * angle
    return np.r_[np.cos(half), np.sin(half) * (phi / angle)]


def _quat_to_rot(q: np.ndarray) -> np.ndarray:
    w, x, y, z = _quat_normalize(q)
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ])


def _yaw_from_rot(R: np.ndarray) -> float:
    return float(np.arctan2(R[1, 0], R[0, 0]))


@dataclass(frozen=True)
class PlanarEstimate:
    """Snapshot of the current estimate. Distances are in metres."""

    timestamp_s: Optional[float]
    position_xy: np.ndarray
    tag_position_xy: np.ndarray
    velocity_xy: np.ndarray
    yaw_rad: float
    covariance_xy: np.ndarray
    std_xy: np.ndarray
    ellipse_95_axes_m: np.ndarray
    ellipse_95_angle_rad: float
    common_range_bias_m: float


class PlanarUwbImuESKF:
    """Error-state EKF for a planar mobile platform with 3D IMU and UWB.

    Parameters
    ----------
    T_body_imu:
        Static 4x4 transform from IMU coordinates to body coordinates.
        Its translation is retained for configuration completeness; the IMU
        acceleration model assumes the IMU origin is the body-origin reference
        (see notes below about a displaced IMU).
    T_body_tag:
        Static transform from UWB tag coordinates to the body frame. The tag
        origin translation is used as the ranging lever arm.
    T_world_anchors:
        Mapping from anchor ID to static 4x4 anchor-to-world transform. The
        translation column gives each anchor's antenna phase-centre position.
    T_world_marker:
        Optional static 4x4 transform from the fixed marker frame into world.
        Required if ``update_marker`` will be used.
    magnetic_field_world:
        Expected Earth's magnetic-field direction in world coordinates. Only
        direction is used. Configure this to match your chosen world heading
        convention and local magnetic declination/dip.
    planar:
        Apply soft pseudo-measurements for body-origin Z=plane_z and Vz=0.
    noise parameters:
        IMU noise values are continuous-time noise densities (per sqrt(Hz));
        UWB and vector-observation standard deviations are ordinary units.

    Notes
    -----
    The IMU sensor-axis rotation is applied to accel/gyro/mag. IMU translation
    is optionally used to subtract tangential and centripetal acceleration at
    the sensor location; this relies on a filtered finite-difference gyro
    derivative and can amplify gyro noise. The UWB tag lever arm is always
    included in every predicted range and its Jacobian.
    """

    ERROR_DIM = 16  # dp3, dv3, dtheta3, dba3, dbg3, common range bias

    def __init__(
        self,
        *,
        T_body_imu: Optional[ArrayLike] = None,
        T_body_tag: Optional[ArrayLike] = None,
        T_world_anchors: Mapping[str, ArrayLike],
        T_world_marker: Optional[ArrayLike] = None,
        plane_z: float = 0.0,
        planar: bool = True,
        gravity_m_s2: float = 9.80665,
        magnetic_field_world: ArrayLike = (1.0, 0.0, 0.0),
        initial_position_xyz: ArrayLike = (0.0, 0.0, 0.0),
        initial_velocity_xyz: ArrayLike = (0.0, 0.0, 0.0),
        initial_orientation_wxyz: Optional[ArrayLike] = None,
        initialize_attitude_from_accel_mag: bool = True,
        initial_position_std_m: ArrayLike = (0.50, 0.50, 0.05),
        initial_velocity_std_m_s: ArrayLike = (0.30, 0.30, 0.15),
        initial_attitude_std_rad: float = np.deg2rad(20.0),
        initial_accel_bias_std_m_s2: float = 0.20,
        initial_gyro_bias_std_rad_s: float = 0.02,
        initial_range_bias_std_m: float = 0.15,
        accel_noise_density: float = 0.08,
        gyro_noise_density: float = 0.003,
        accel_bias_rw_density: float = 0.0005,
        gyro_bias_rw_density: float = 0.00005,
        range_bias_rw_density_m_sqrt_s: float = 0.001,
        uwb_range_std_m: float = 0.015,
        marker_position_std_m: float = 0.015,
        marker_orientation_std_rad: float = np.deg2rad(2.0),
        marker_gate_sigma: float = 8.0,
        magnetometer_direction_std: float = 0.08,
        gravity_direction_std: float = 0.05,
        mag_norm_range_uT: Optional[Tuple[float, float]] = (10.0, 100.0),
        gravity_norm_tolerance: float = 0.15,
        use_magnetometer_update: bool = True,
        use_gravity_update: bool = True,
        plane_position_std_m: float = 0.01,
        plane_velocity_std_m_s: float = 0.03,
        range_gate_sigma: float = 6.0,
        range_huber_sigma: float = 2.5,
        max_propagation_step_s: float = 0.01,
        compensate_imu_lever_arm: bool = False,
        angular_acceleration_filter_alpha: float = 0.25,
        residual_history_size: int = 500,
    ) -> None:
        if not T_world_anchors:
            raise ValueError("T_world_anchors must contain at least three anchors")
        if len(T_world_anchors) < 3:
            raise ValueError("At least three UWB anchors are required")

        self.T_body_imu = _transform(T_body_imu, "T_body_imu")
        self.T_body_tag = _transform(T_body_tag, "T_body_tag")
        self.T_world_marker = (
            None if T_world_marker is None
            else _transform(T_world_marker, "T_world_marker")
        )
        self.T_world_anchors: Dict[str, np.ndarray] = {
            str(k): _transform(v, f"T_world_anchors[{k!r}]")
            for k, v in T_world_anchors.items()
        }
        self.anchor_positions_world = {
            k: T[:3, 3].copy() for k, T in self.T_world_anchors.items()
        }
        anchor_xy = np.array([p[:2] for p in self.anchor_positions_world.values()])
        if np.linalg.matrix_rank(anchor_xy - anchor_xy.mean(axis=0), tol=1e-6) < 2:
            raise ValueError("Anchor XY positions must span 2D; collinear anchors are degenerate")

        self.R_body_imu = self.T_body_imu[:3, :3]
        self.r_body_imu = self.T_body_imu[:3, 3].copy()
        self.r_body_tag = self.T_body_tag[:3, 3].copy()
        self.compensate_imu_lever_arm = bool(compensate_imu_lever_arm)
        self.angular_acceleration_filter_alpha = float(angular_acceleration_filter_alpha)
        if not 0.0 < self.angular_acceleration_filter_alpha <= 1.0:
            raise ValueError("angular_acceleration_filter_alpha must be in (0, 1]")
        self.plane_z = float(plane_z)
        self.planar = bool(planar)
        self.g = float(gravity_m_s2)
        if self.g <= 0:
            raise ValueError("gravity_m_s2 must be positive")

        m_world = _vec(magnetic_field_world, 3, "magnetic_field_world")
        self.magnetic_field_world = _normalize(m_world)
        if self.magnetic_field_world is None:
            raise ValueError("magnetic_field_world must be non-zero")

        self.initialize_attitude_from_accel_mag = bool(initialize_attitude_from_accel_mag)
        self.use_magnetometer_update = bool(use_magnetometer_update)
        self.use_gravity_update = bool(use_gravity_update)
        self.mag_norm_range_uT = mag_norm_range_uT
        self.gravity_norm_tolerance = float(gravity_norm_tolerance)

        self.accel_noise_density = float(accel_noise_density)
        self.gyro_noise_density = float(gyro_noise_density)
        self.accel_bias_rw_density = float(accel_bias_rw_density)
        self.gyro_bias_rw_density = float(gyro_bias_rw_density)
        self.range_bias_rw_density = float(range_bias_rw_density_m_sqrt_s)
        self.uwb_range_std_m = float(uwb_range_std_m)
        self.marker_position_std_m = float(marker_position_std_m)
        self.marker_orientation_std_rad = float(marker_orientation_std_rad)
        self.marker_gate_sigma = float(marker_gate_sigma)
        if (self.marker_position_std_m <= 0 or
                self.marker_orientation_std_rad <= 0 or
                self.marker_gate_sigma <= 0):
            raise ValueError("Marker noise values and marker_gate_sigma must be positive")
        self.marker_updates = 0
        self.marker_accepted = 0
        self.marker_rejected = 0
        self._last_marker_residual = None
        self._last_marker_nis = None
        self.mag_direction_std = float(magnetometer_direction_std)
        self.gravity_direction_std = float(gravity_direction_std)
        self.plane_position_std = float(plane_position_std_m)
        self.plane_velocity_std = float(plane_velocity_std_m_s)
        self.range_gate_sigma = float(range_gate_sigma)
        self.range_huber_sigma = float(range_huber_sigma)
        self.max_step = float(max_propagation_step_s)
        if self.max_step <= 0:
            raise ValueError("max_propagation_step_s must be positive")
        if min(self.accel_noise_density, self.gyro_noise_density,
               self.accel_bias_rw_density, self.gyro_bias_rw_density,
               self.range_bias_rw_density, self.uwb_range_std_m,
               self.mag_direction_std, self.gravity_direction_std,
               self.plane_position_std, self.plane_velocity_std) < 0:
            raise ValueError("Noise standard deviations/densities cannot be negative")

        self.p = _vec(initial_position_xyz, 3, "initial_position_xyz")
        self.v = _vec(initial_velocity_xyz, 3, "initial_velocity_xyz")
        self.q = (np.array([1.0, 0.0, 0.0, 0.0]) if initial_orientation_wxyz is None
                  else _quat_normalize(_vec(initial_orientation_wxyz, 4, "initial_orientation_wxyz")))
        self.ba = np.zeros(3)  # accelerometer bias, body frame
        self.bg = np.zeros(3)  # gyro bias, body frame
        self.range_bias = 0.0  # shared additive range bias, metres

        P = np.zeros((self.ERROR_DIM, self.ERROR_DIM))
        P[0:3, 0:3] = np.diag(_vec(initial_position_std_m, 3, "initial_position_std_m") ** 2)
        P[3:6, 3:6] = np.diag(_vec(initial_velocity_std_m_s, 3, "initial_velocity_std_m_s") ** 2)
        P[6:9, 6:9] = np.eye(3) * float(initial_attitude_std_rad) ** 2
        P[9:12, 9:12] = np.eye(3) * float(initial_accel_bias_std_m_s2) ** 2
        P[12:15, 12:15] = np.eye(3) * float(initial_gyro_bias_std_rad_s) ** 2
        P[15, 15] = float(initial_range_bias_std_m) ** 2
        self.P = P

        self.timestamp_s: Optional[float] = None
        self._last_accel_body: Optional[np.ndarray] = None
        self._last_gyro_body: Optional[np.ndarray] = None
        self._last_imu_timestamp_s: Optional[float] = None
        self._filtered_angular_acceleration = np.zeros(3)
        self._attitude_initialized = initial_orientation_wxyz is not None

        self.range_updates = 0
        self.range_accepted = 0
        self.range_rejected = 0
        self._range_residuals: Deque[float] = deque(maxlen=int(residual_history_size))
        self._range_nis: Deque[float] = deque(maxlen=int(residual_history_size))

    # ------------------------------------------------------------------
    # Public sensor update API
    # ------------------------------------------------------------------
    def update_imu(
        self,
        *,
        timestamp_s: float,
        accel_m_s2: ArrayLike,
        gyro_rad_s: ArrayLike,
        mag_uT: Optional[ArrayLike] = None,
    ) -> None:
        """Consume one 3-axis accelerometer/gyroscope sample and optional mag.

        ``timestamp_s`` must use the same monotonic clock as UWB measurements.
        The filter integrates the previously received IMU sample up to this
        timestamp, then stores the new sample for subsequent propagation.
        """
        ts = float(timestamp_s)
        if not np.isfinite(ts):
            raise ValueError("timestamp_s must be finite")
        accel_sensor = _vec(accel_m_s2, 3, "accel_m_s2")
        gyro_sensor = _vec(gyro_rad_s, 3, "gyro_rad_s")
        accel_body_raw = self.R_body_imu @ accel_sensor
        gyro_body = self.R_body_imu @ gyro_sensor
        accel_body = accel_body_raw.copy()
        if self.compensate_imu_lever_arm and np.linalg.norm(self.r_body_imu) > 1e-9:
            if self._last_gyro_body is not None and self._last_imu_timestamp_s is not None:
                sample_dt = ts - self._last_imu_timestamp_s
                if sample_dt > 1e-6:
                    alpha_raw = (gyro_body - self._last_gyro_body) / sample_dt
                    a = self.angular_acceleration_filter_alpha
                    self._filtered_angular_acceleration = (
                        a * alpha_raw + (1.0 - a) * self._filtered_angular_acceleration
                    )
            omega_for_lever = gyro_body - self.bg
            rotational_accel = (
                np.cross(self._filtered_angular_acceleration, self.r_body_imu)
                + np.cross(omega_for_lever, np.cross(omega_for_lever, self.r_body_imu))
            )
            accel_body -= rotational_accel
        mag_body = None
        mag_norm = None
        if mag_uT is not None:
            mag_sensor = _vec(mag_uT, 3, "mag_uT")
            mag_body = self.R_body_imu @ mag_sensor
            mag_norm = float(np.linalg.norm(mag_sensor))

        if self.timestamp_s is None:
            self.timestamp_s = ts
            if (not self._attitude_initialized and self.initialize_attitude_from_accel_mag
                    and mag_body is not None):
                self._try_initialize_attitude(accel_body, mag_body)
        else:
            self._propagate_to(ts)

        self._last_accel_body = accel_body
        self._last_gyro_body = gyro_body
        self._last_imu_timestamp_s = ts

        # Low-dynamic gravity direction observation (roll/pitch correction).
        accel_norm = float(np.linalg.norm(accel_body - self.ba))
        if (self.use_gravity_update and
                abs(accel_norm - self.g) <= self.gravity_norm_tolerance * self.g):
            measured_up_body = _normalize(accel_body - self.ba)
            if measured_up_body is not None:
                predicted_up_body = _quat_to_rot(self.q).T @ np.array([0.0, 0.0, 1.0])
                H = np.zeros((3, self.ERROR_DIM))
                H[:, 6:9] = _skew(predicted_up_body)
                self._measurement_update(
                    measured_up_body - predicted_up_body,
                    H,
                    np.eye(3) * self.gravity_direction_std ** 2,
                )

        # Magnetometer direction observation (heading correction).
        mag_valid = mag_body is not None
        if mag_valid and self.mag_norm_range_uT is not None:
            lo, hi = self.mag_norm_range_uT
            mag_valid = float(lo) <= float(mag_norm) <= float(hi)
        if self.use_magnetometer_update and mag_valid:
            measured_mag_body = _normalize(mag_body)
            if measured_mag_body is not None:
                predicted_mag_body = _quat_to_rot(self.q).T @ self.magnetic_field_world
                predicted_mag_body = _normalize(predicted_mag_body)
                if predicted_mag_body is not None:
                    H = np.zeros((3, self.ERROR_DIM))
                    H[:, 6:9] = _skew(predicted_mag_body)
                    self._measurement_update(
                        measured_mag_body - predicted_mag_body,
                        H,
                        np.eye(3) * self.mag_direction_std ** 2,
                    )

        self._apply_planar_constraint()

    def update_range(
        self,
        *,
        anchor_id: str,
        distance_m: float,
        timestamp_s: Optional[float] = None,
        std_m: Optional[float] = None,
    ) -> bool:
        """Consume one UWB range. Returns True if the range was accepted.

        Ranges can be supplied one anchor at a time and at their own capture
        timestamps. Out-of-order timestamps raise ValueError rather than being
        silently fused against the wrong state time.
        """
        key = str(anchor_id)
        if key not in self.anchor_positions_world:
            raise KeyError(f"Unknown anchor_id {anchor_id!r}")
        z = float(distance_m)
        if not np.isfinite(z) or z <= 0:
            raise ValueError("distance_m must be a finite positive distance")
        sigma = self.uwb_range_std_m if std_m is None else float(std_m)
        if not np.isfinite(sigma) or sigma <= 0:
            raise ValueError("std_m must be finite and positive")

        if timestamp_s is not None:
            ts = float(timestamp_s)
            if not np.isfinite(ts):
                raise ValueError("timestamp_s must be finite")
            if self.timestamp_s is None:
                self.timestamp_s = ts
            else:
                self._propagate_to(ts)
        elif self.timestamp_s is None:
            raise ValueError("Provide timestamp_s until the filter has a time reference")

        Rwb = _quat_to_rot(self.q)
        tag_world = self.p + Rwb @ self.r_body_tag
        anchor_world = self.anchor_positions_world[key]
        delta = tag_world - anchor_world
        predicted_distance = float(np.linalg.norm(delta))
        if predicted_distance < 1e-9:
            return False
        line_of_sight = delta / predicted_distance
        predicted_range = predicted_distance + self.range_bias
        innovation = z - predicted_range

        H = np.zeros((1, self.ERROR_DIM))
        H[0, 0:3] = line_of_sight
        H[0, 6:9] = line_of_sight @ (-Rwb @ _skew(self.r_body_tag))
        H[0, 15] = 1.0
        Rm = np.array([[sigma ** 2]])
        S = float((H @ self.P @ H.T)[0, 0] + Rm[0, 0])
        if not np.isfinite(S) or S <= 0:
            return False
        normalized_innovation = abs(innovation) / np.sqrt(S)
        self.range_updates += 1
        if normalized_innovation > self.range_gate_sigma:
            self.range_rejected += 1
            self._range_residuals.append(float(innovation))
            self._range_nis.append(float(innovation ** 2 / S))
            return False

        # Huber-style variance inflation limits the influence of moderate
        # multipath/NLOS residuals without immediately discarding them.
        if normalized_innovation > self.range_huber_sigma:
            inflation = (normalized_innovation / self.range_huber_sigma) ** 2
            Rm *= inflation

        accepted = self._measurement_update(np.array([innovation]), H, Rm)
        self._range_residuals.append(float(innovation))
        self._range_nis.append(float(innovation ** 2 / S))
        if accepted:
            self.range_accepted += 1
        else:
            self.range_rejected += 1
        self._apply_planar_constraint()
        return accepted

    def update_ranges(
        self,
        ranges_m: Mapping[str, float],
        *,
        timestamp_s: Optional[float] = None,
        std_m: Optional[float] = None,
    ) -> Dict[str, bool]:
        """Convenience method for a group of ranges sharing one timestamp."""
        return {
            anchor_id: self.update_range(
                anchor_id=anchor_id,
                distance_m=distance,
                timestamp_s=timestamp_s,
                std_m=std_m,
            )
            for anchor_id, distance in ranges_m.items()
        }

    # ------------------------------------------------------------------
    # Estimate and uncertainty API
    # ------------------------------------------------------------------
    def update_marker(
        self,
        *,
        T_body_marker: ArrayLike,
        timestamp_s: Optional[float] = None,
        position_std_m: Optional[float] = None,
        orientation_std_rad: Optional[float] = None,
    ) -> bool:
        """Fuse one externally detected fixed-marker pose observation.

        ``T_body_marker`` maps marker-frame coordinates into body-frame
        coordinates. For a camera detection that returns ``T_camera_marker``,
        compose the calibrated camera-to-body extrinsic before calling:

            T_body_marker = T_body_camera @ T_camera_marker

        The camera/servo transform is therefore not separately needed by this
        filter. ``T_world_marker`` must have been supplied in the constructor.

        The observation is converted into a measured world-body pose using
        ``T_world_body_meas = T_world_marker @ inv(T_body_marker)``.
        Returns False when the innovation is non-finite, singular, or rejected
        by the marker innovation gate.
        """
        if self.T_world_marker is None:
            raise RuntimeError(
                "update_marker requires T_world_marker in the constructor"
            )

        T_bm = _transform(T_body_marker, "T_body_marker")
        if timestamp_s is not None:
            ts = float(timestamp_s)
            if not np.isfinite(ts):
                raise ValueError("timestamp_s must be finite")
            if self.timestamp_s is None:
                self.timestamp_s = ts
            else:
                self._propagate_to(ts)
        elif self.timestamp_s is None:
            raise ValueError("Provide timestamp_s until the filter has a time reference")

        T_wb_meas = self.T_world_marker @ np.linalg.inv(T_bm)
        R_wb_meas = T_wb_meas[:3, :3]
        p_wb_meas = T_wb_meas[:3, 3]

        R_wb_pred = _quat_to_rot(self.q)
        position_residual = p_wb_meas - self.p
        R_error = R_wb_pred.T @ R_wb_meas
        orientation_residual = self._rotation_matrix_to_rotvec(R_error)
        residual = np.r_[position_residual, orientation_residual]

        H = np.zeros((6, self.ERROR_DIM))
        H[0:3, 0:3] = np.eye(3)
        H[3:6, 6:9] = np.eye(3)

        pos_sigma = (
            self.marker_position_std_m if position_std_m is None
            else float(position_std_m)
        )
        rot_sigma = (
            self.marker_orientation_std_rad if orientation_std_rad is None
            else float(orientation_std_rad)
        )
        if not np.isfinite(pos_sigma) or pos_sigma <= 0:
            raise ValueError("position_std_m must be finite and positive")
        if not np.isfinite(rot_sigma) or rot_sigma <= 0:
            raise ValueError("orientation_std_rad must be finite and positive")

        Rm = np.diag([pos_sigma ** 2] * 3 + [rot_sigma ** 2] * 3)
        S = H @ self.P @ H.T + Rm
        try:
            nis = float(residual @ np.linalg.solve(S, residual))
        except np.linalg.LinAlgError:
            return False

        self.marker_updates += 1
        self._last_marker_residual = residual.copy()
        self._last_marker_nis = nis
        if not np.isfinite(nis) or np.sqrt(max(nis, 0.0)) > self.marker_gate_sigma:
            self.marker_rejected += 1
            return False

        accepted = self._measurement_update(residual, H, Rm)
        if accepted:
            self.marker_accepted += 1
            self._apply_planar_constraint()
        else:
            self.marker_rejected += 1
        return accepted

    @staticmethod
    def _rotation_matrix_to_rotvec(R: np.ndarray) -> np.ndarray:
        """SO(3) logarithm map, returning an axis-angle rotation vector."""
        cos_angle = float(np.clip((np.trace(R) - 1.0) * 0.5, -1.0, 1.0))
        angle = float(np.arccos(cos_angle))
        vee = np.array([
            R[2, 1] - R[1, 2],
            R[0, 2] - R[2, 0],
            R[1, 0] - R[0, 1],
        ])
        if angle < 1e-7:
            return 0.5 * vee
        sin_angle = float(np.sin(angle))
        if abs(sin_angle) < 1e-7:
            # Stable axis extraction near 180 degrees.
            eigvals, eigvecs = np.linalg.eig(R)
            axis = np.real(eigvecs[:, int(np.argmin(np.abs(eigvals - 1.0)))])
            axis = axis / max(float(np.linalg.norm(axis)), 1e-12)
            return angle * axis
        return (angle / (2.0 * sin_angle)) * vee

    def get_estimate(self) -> PlanarEstimate:
        """Return the current 2D position, tag position, and covariance."""
        Rwb = _quat_to_rot(self.q)
        tag_world = self.p + Rwb @ self.r_body_tag
        Pxy = self.P[0:2, 0:2].copy()
        Pxy = 0.5 * (Pxy + Pxy.T)
        eigvals, eigvecs = np.linalg.eigh(Pxy)
        eigvals = np.maximum(eigvals, 0.0)
        order = np.argsort(eigvals)[::-1]
        eigvals = eigvals[order]
        eigvecs = eigvecs[:, order]
        chi2_95_df2 = 5.991464547
        axes_95 = np.sqrt(eigvals * chi2_95_df2)
        major_vec = eigvecs[:, 0]
        angle = float(np.arctan2(major_vec[1], major_vec[0]))
        return PlanarEstimate(
            timestamp_s=self.timestamp_s,
            position_xy=self.p[:2].copy(),
            tag_position_xy=tag_world[:2].copy(),
            velocity_xy=self.v[:2].copy(),
            yaw_rad=_yaw_from_rot(Rwb),
            covariance_xy=Pxy,
            std_xy=np.sqrt(np.maximum(np.diag(Pxy), 0.0)),
            ellipse_95_axes_m=axes_95,
            ellipse_95_angle_rad=angle,
            common_range_bias_m=float(self.range_bias),
        )

    def get_error_statistics(self) -> dict:
        """Return estimated uncertainty and recent UWB residual diagnostics.

        These are filter-estimated uncertainties and innovation statistics,
        not ground-truth position error. Actual RMSE requires an external truth
        trajectory and comparison against ``get_estimate().position_xy``.
        """
        est = self.get_estimate()
        residuals = np.asarray(self._range_residuals, dtype=float)
        nis = np.asarray(self._range_nis, dtype=float)
        return {
            "timestamp_s": self.timestamp_s,
            "position_covariance_xy_m2": est.covariance_xy.copy(),
            "position_std_xy_m": est.std_xy.copy(),
            "horizontal_rms_1sigma_m": float(np.sqrt(np.trace(est.covariance_xy))),
            "ellipse_95_axes_m": est.ellipse_95_axes_m.copy(),
            "ellipse_95_angle_rad": est.ellipse_95_angle_rad,
            "range_updates": self.range_updates,
            "range_accepted": self.range_accepted,
            "range_rejected": self.range_rejected,
            "recent_range_residual_rms_m": (
                float(np.sqrt(np.mean(residuals ** 2))) if residuals.size else None
            ),
            "recent_range_innovation_nis_mean": (
                float(np.mean(nis)) if nis.size else None
            ),
            "common_range_bias_estimate_m": float(self.range_bias),
            "marker_updates": self.marker_updates,
            "marker_accepted": self.marker_accepted,
            "marker_rejected": self.marker_rejected,
            "last_marker_residual": (
                None if self._last_marker_residual is None
                else self._last_marker_residual.copy()
            ),
            "last_marker_innovation_nis": self._last_marker_nis,
        }

    def reset(
        self,
        *,
        position_xyz: ArrayLike = (0.0, 0.0, 0.0),
        velocity_xyz: ArrayLike = (0.0, 0.0, 0.0),
        orientation_wxyz: Optional[ArrayLike] = None,
        timestamp_s: Optional[float] = None,
    ) -> None:
        """Reset the nominal state and timing while retaining configuration."""
        self.p = _vec(position_xyz, 3, "position_xyz")
        self.v = _vec(velocity_xyz, 3, "velocity_xyz")
        self.q = (np.array([1.0, 0.0, 0.0, 0.0]) if orientation_wxyz is None
                  else _quat_normalize(_vec(orientation_wxyz, 4, "orientation_wxyz")))
        self.ba[:] = 0.0
        self.bg[:] = 0.0
        self.range_bias = 0.0
        self.timestamp_s = None if timestamp_s is None else float(timestamp_s)
        self._last_accel_body = None
        self._last_gyro_body = None
        self._last_imu_timestamp_s = None
        self._filtered_angular_acceleration[:] = 0.0
        self._attitude_initialized = orientation_wxyz is not None
        self.range_updates = self.range_accepted = self.range_rejected = 0
        self.marker_updates = self.marker_accepted = self.marker_rejected = 0
        self._last_marker_residual = None
        self._last_marker_nis = None
        self._range_residuals.clear()
        self._range_nis.clear()
        self.P = np.eye(self.ERROR_DIM) * 1e-6
        self.P[0:3, 0:3] = np.diag([0.50 ** 2, 0.50 ** 2, 0.05 ** 2])
        self.P[3:6, 3:6] = np.diag([0.30 ** 2, 0.30 ** 2, 0.15 ** 2])
        self.P[6:9, 6:9] = np.eye(3) * np.deg2rad(20.0) ** 2
        self.P[9:12, 9:12] = np.eye(3) * 0.20 ** 2
        self.P[12:15, 12:15] = np.eye(3) * 0.02 ** 2
        self.P[15, 15] = 0.15 ** 2

    # ------------------------------------------------------------------
    # Internal mechanics
    # ------------------------------------------------------------------
    def _try_initialize_attitude(self, accel_body: np.ndarray, mag_body: np.ndarray) -> None:
        up_b = _normalize(accel_body)
        mag_b = _normalize(mag_body)
        if up_b is None or mag_b is None:
            return
        b1 = _normalize(np.cross(up_b, mag_b))
        w1 = _normalize(np.cross(np.array([0.0, 0.0, 1.0]), self.magnetic_field_world))
        if b1 is None or w1 is None:
            return
        b2 = np.cross(up_b, b1)
        up_w = np.array([0.0, 0.0, 1.0])
        w2 = np.cross(up_w, w1)
        Rwb = np.column_stack((w1, w2, up_w)) @ np.column_stack((b1, b2, up_b)).T
        # Numerically project to a proper rotation before converting to q.
        U, _, Vt = np.linalg.svd(Rwb)
        Rwb = U @ np.diag([1.0, 1.0, np.linalg.det(U @ Vt)]) @ Vt
        self.q = self._rot_to_quat(Rwb)
        self._attitude_initialized = True

    @staticmethod
    def _rot_to_quat(R: np.ndarray) -> np.ndarray:
        # Stable matrix-to-quaternion conversion; result is [w, x, y, z].
        tr = float(np.trace(R))
        if tr > 0.0:
            s = np.sqrt(tr + 1.0) * 2.0
            q = np.array([0.25 * s,
                          (R[2, 1] - R[1, 2]) / s,
                          (R[0, 2] - R[2, 0]) / s,
                          (R[1, 0] - R[0, 1]) / s])
        else:
            i = int(np.argmax(np.diag(R)))
            if i == 0:
                s = np.sqrt(1.0 + R[0, 0] - R[1, 1] - R[2, 2]) * 2.0
                q = np.array([(R[2, 1] - R[1, 2]) / s, 0.25 * s,
                              (R[0, 1] + R[1, 0]) / s, (R[0, 2] + R[2, 0]) / s])
            elif i == 1:
                s = np.sqrt(1.0 + R[1, 1] - R[0, 0] - R[2, 2]) * 2.0
                q = np.array([(R[0, 2] - R[2, 0]) / s,
                              (R[0, 1] + R[1, 0]) / s, 0.25 * s,
                              (R[1, 2] + R[2, 1]) / s])
            else:
                s = np.sqrt(1.0 + R[2, 2] - R[0, 0] - R[1, 1]) * 2.0
                q = np.array([(R[1, 0] - R[0, 1]) / s,
                              (R[0, 2] + R[2, 0]) / s,
                              (R[1, 2] + R[2, 1]) / s, 0.25 * s])
        return _quat_normalize(q)

    def _propagate_to(self, timestamp_s: float) -> None:
        ts = float(timestamp_s)
        if self.timestamp_s is None:
            self.timestamp_s = ts
            return
        dt_total = ts - self.timestamp_s
        if dt_total < -1e-9:
            raise ValueError(
                f"Out-of-order measurement: {ts:.9f}s is before filter time "
                f"{self.timestamp_s:.9f}s"
            )
        if dt_total <= 0.0:
            return

        if self._last_accel_body is not None and self._last_gyro_body is not None:
            remaining = dt_total
            while remaining > 1e-12:
                dt = min(remaining, self.max_step)
                self._propagate_step(dt, self._last_accel_body, self._last_gyro_body)
                remaining -= dt
        self.timestamp_s = ts
        self._apply_planar_constraint()

    def _propagate_step(self, dt: float, accel_body: np.ndarray, gyro_body: np.ndarray) -> None:
        Rwb = _quat_to_rot(self.q)
        specific_force = accel_body - self.ba
        omega = gyro_body - self.bg
        accel_world = Rwb @ specific_force + np.array([0.0, 0.0, -self.g])

        self.p += self.v * dt + 0.5 * accel_world * dt * dt
        self.v += accel_world * dt
        self.q = _quat_normalize(_quat_mul(self.q, _quat_from_rotvec(omega * dt)))

        # Continuous-time error-state Jacobian for right-multiplicative attitude error.
        Fc = np.zeros((self.ERROR_DIM, self.ERROR_DIM))
        Fc[0:3, 3:6] = np.eye(3)
        Fc[3:6, 6:9] = -Rwb @ _skew(specific_force)
        Fc[3:6, 9:12] = -Rwb
        Fc[6:9, 6:9] = -_skew(omega)
        Fc[6:9, 12:15] = -np.eye(3)
        Phi = np.eye(self.ERROR_DIM) + Fc * dt + 0.5 * (Fc @ Fc) * dt * dt

        # Noise injection: accel, gyro, accel-bias RW, gyro-bias RW, range-bias RW.
        G = np.zeros((self.ERROR_DIM, 13))
        G[3:6, 0:3] = -Rwb
        G[6:9, 3:6] = -np.eye(3)
        G[9:12, 6:9] = np.eye(3)
        G[12:15, 9:12] = np.eye(3)
        G[15, 12] = 1.0
        q_diag = np.r_[
            np.full(3, self.accel_noise_density ** 2),
            np.full(3, self.gyro_noise_density ** 2),
            np.full(3, self.accel_bias_rw_density ** 2),
            np.full(3, self.gyro_bias_rw_density ** 2),
            self.range_bias_rw_density ** 2,
        ]
        Qc = np.diag(q_diag)
        self.P = Phi @ self.P @ Phi.T + G @ Qc @ G.T * dt
        self.P = 0.5 * (self.P + self.P.T)

    def _measurement_update(self, residual: np.ndarray, H: np.ndarray, Rm: np.ndarray) -> bool:
        residual = np.asarray(residual, dtype=float).reshape(-1)
        H = np.asarray(H, dtype=float)
        Rm = np.asarray(Rm, dtype=float)
        if not np.all(np.isfinite(residual)) or not np.all(np.isfinite(H)):
            return False
        S = H @ self.P @ H.T + Rm
        try:
            K = np.linalg.solve(S, H @ self.P).T
        except np.linalg.LinAlgError:
            return False
        dx = K @ residual

        self.p += dx[0:3]
        self.v += dx[3:6]
        dtheta = dx[6:9]
        self.q = _quat_normalize(_quat_mul(self.q, _quat_from_rotvec(dtheta)))
        self.ba += dx[9:12]
        self.bg += dx[12:15]
        self.range_bias += float(dx[15])

        I_KH = np.eye(self.ERROR_DIM) - K @ H
        # Joseph form preserves symmetry/positive-semidefiniteness better.
        self.P = I_KH @ self.P @ I_KH.T + K @ Rm @ K.T
        # Reset Jacobian for the injected right-multiplicative attitude error.
        reset = np.eye(self.ERROR_DIM)
        reset[6:9, 6:9] -= 0.5 * _skew(dtheta)
        self.P = reset @ self.P @ reset.T
        self.P = 0.5 * (self.P + self.P.T)
        return True

    def _apply_planar_constraint(self) -> None:
        if not self.planar:
            return
        H = np.zeros((2, self.ERROR_DIM))
        H[0, 2] = 1.0
        H[1, 5] = 1.0
        residual = np.array([self.plane_z - self.p[2], -self.v[2]])
        Rm = np.diag([self.plane_position_std ** 2, self.plane_velocity_std ** 2])
        self._measurement_update(residual, H, Rm)


__all__ = ["PlanarUwbImuESKF", "PlanarEstimate"]
