/*
 *  gl_clip_plane_policy.h - OpenGL user clip-plane transform helpers.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef GL_CLIP_PLANE_POLICY_H
#define GL_CLIP_PLANE_POLICY_H

#include <cmath>

static inline bool GLTransformClipPlaneToEyeSpace(double out[4],
                                                  const double plane[4],
                                                  const float modelview[16])
{
	if (!out || !plane || !modelview)
		return false;

	const double a00 = modelview[0];
	const double a01 = modelview[4];
	const double a02 = modelview[8];
	const double a10 = modelview[1];
	const double a11 = modelview[5];
	const double a12 = modelview[9];
	const double a20 = modelview[2];
	const double a21 = modelview[6];
	const double a22 = modelview[10];

	const double det =
		a00 * (a11 * a22 - a12 * a21) -
		a01 * (a10 * a22 - a12 * a20) +
		a02 * (a10 * a21 - a11 * a20);
	if (std::fabs(det) <= 1.0e-20)
		return false;

	const double inv_det = 1.0 / det;
	const double inv00 =  (a11 * a22 - a12 * a21) * inv_det;
	const double inv01 =  (a02 * a21 - a01 * a22) * inv_det;
	const double inv02 =  (a01 * a12 - a02 * a11) * inv_det;
	const double inv10 =  (a12 * a20 - a10 * a22) * inv_det;
	const double inv11 =  (a00 * a22 - a02 * a20) * inv_det;
	const double inv12 =  (a02 * a10 - a00 * a12) * inv_det;
	const double inv20 =  (a10 * a21 - a11 * a20) * inv_det;
	const double inv21 =  (a01 * a20 - a00 * a21) * inv_det;
	const double inv22 =  (a00 * a11 - a01 * a10) * inv_det;

	const double nx = inv00 * plane[0] + inv10 * plane[1] + inv20 * plane[2];
	const double ny = inv01 * plane[0] + inv11 * plane[1] + inv21 * plane[2];
	const double nz = inv02 * plane[0] + inv12 * plane[1] + inv22 * plane[2];

	out[0] = nx;
	out[1] = ny;
	out[2] = nz;
	out[3] = plane[3] -
		(nx * modelview[12] + ny * modelview[13] + nz * modelview[14]);
	return true;
}

#endif
