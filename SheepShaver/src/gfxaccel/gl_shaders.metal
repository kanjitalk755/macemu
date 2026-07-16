/*
 *  gl_shaders.metal - OpenGL 1.2 FFP uber-shader via uniforms
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  Implements the full OpenGL 1.2 fixed-function pipeline as a pair of
 *  Metal vertex/fragment shaders controlled by uniform buffers:
 *    - MVP + modelview + normal matrix transforms
 *    - Per-vertex Phong lighting for up to 8 lights
 *    - 4 texture environment modes (modulate, decal, blend, replace)
 *    - 3 fog modes (linear, exp, exp2)
 *    - Alpha test with all 8 comparison functions
 *    - Flat/smooth shading support
 */

#include <metal_stdlib>
using namespace metal;


// ---- Vertex input/output ----

struct GLVertexIn {
    float4 position        [[attribute(0)]];
    float4 color           [[attribute(1)]];
    float3 normal          [[attribute(2)]];
    float3 texcoord        [[attribute(3)]];   // (s, t, q) unit 0 — q enables projective texturing
    float3 texcoord1       [[attribute(4)]];   // (s, t, q) unit 1 — for multitexture
    float3 secondary_color [[attribute(5)]];   // (r, g, b) EXT_secondary_color / GL_COLOR_SUM
};

struct GLVertexOut {
    float4 position     [[position]];
    float  point_size   [[point_size]];
    float4 color;
    float3 eye_normal;
    float3 texcoord;    // (s, t, q) unit 0 — interpolated, then divided per-fragment
    float3 texcoord1;   // (s, t, q) unit 1 — for multitexture
    float3 secondary_color; // (r, g, b) EXT_secondary_color / GL_COLOR_SUM — added after texturing
    float3 eye_position;
    float  fog_factor;
    float  clip_dist0;  // user clip plane distances (interpolated, discard if < 0)
    float  clip_dist1;
    float  clip_dist2;
    float  clip_dist3;
    float  clip_dist4;
    float  clip_dist5;
    int    num_clip_planes; // number of active clip planes
};


// ---- Uniform buffers ----

struct GLVertexUniforms {
    float4x4 mvp_matrix;
    float4x4 modelview_matrix;
    float3x3 normal_matrix;
    int      lighting_enabled;
    int      normalize_enabled;
    int      num_active_lights;
    int      fog_enabled;
    int      fog_mode;          // 0=none, 1=linear, 2=exp, 3=exp2
    float    fog_start;
    float    fog_end;
    float    fog_density;
    float    point_size;         // glPointSize value (default 1.0)
    int      two_side_lighting; // GL_LIGHT_MODEL_TWO_SIDE: negate normal for back-facing vertices
    // User clip planes (GL_CLIP_PLANE0-5). Equation is dot(plane, eye_pos) >= 0.
    int      num_clip_planes;   // number of enabled clip planes (0-6)
    float4   clip_planes[6];    // plane equations in eye space
};

struct GLFragmentUniforms {
    int    texenv_mode;         // 0=modulate, 1=decal, 2=blend, 3=replace
    float4 texenv_color;
    float4 fog_color;
    int    alpha_test_enabled;
    int    alpha_func;          // 0=never,1=less,2=equal,3=lequal,4=greater,5=notequal,6=gequal,7=always
    float  alpha_ref;
    int    has_texture;
    int    has_texture_3d;      // 1 = bound texture is 3D, sample from tex3d at index 1
    int    shade_model;         // 0=flat, 1=smooth
    int    color_sum_enabled;   // GL_COLOR_SUM (EXT_secondary_color): add secondary color after texturing
    int    has_texture_unit1;   // ARB_multitexture: unit 1 has a bound+enabled 2D texture (sample tex1 with texcoord1)
    int    texenv1_mode;        // unit-1 texenv mode (0=modulate,1=decal,2=blend,3=replace,4=add)
    int    force_opaque_output; // window overlay, GL blending disabled: store coverage alpha
};

struct GLLight {
    float4 ambient;
    float4 diffuse;
    float4 specular;
    float4 position;            // eye-space (transformed at glLight time)
    float3 spot_direction;
    float  spot_exponent;
    float  spot_cutoff;         // cos(cutoff) for comparison, or -1.0 if 180 degrees
    float  constant_atten;
    float  linear_atten;
    float  quadratic_atten;
    int    enabled;
    float  _pad0;               // padding to align to 16 bytes
    float  _pad1;
    float  _pad2;
};

struct GLMaterialData {
    float4 ambient;
    float4 diffuse;
    float4 specular;
    float4 emission;
    float  shininess;
    float  _pad0;
    float  _pad1;
    float  _pad2;
};

struct GLLightingData {
    GLLight    lights[8];
    GLMaterialData material;
    float4     global_ambient;
};


// ---- Vertex shader ----

vertex GLVertexOut gl_vertex_main(
    GLVertexIn in [[stage_in]],
    constant GLVertexUniforms &uniforms [[buffer(1)]],
    constant GLLightingData &lighting [[buffer(2)]])
{
    GLVertexOut out;

    // 1. Transform position
    out.position = uniforms.mvp_matrix * in.position;

    // Remap depth from GL NDC [-1,+1] to Metal NDC [0,+1].
    out.position.z = out.position.z * 0.5 + out.position.w * 0.5;

    // Point size for GL_POINTS primitives
    out.point_size = uniforms.point_size;

    // 2. Eye-space position (for fog distance and lighting)
    float4 eye_pos = uniforms.modelview_matrix * in.position;
    out.eye_position = eye_pos.xyz;

    // 3. Transform and optionally normalize the normal
    float3 eye_normal = uniforms.normal_matrix * in.normal;
    if (uniforms.normalize_enabled) {
        float len = length(eye_normal);
        if (len > 0.0) eye_normal /= len;
    }
    // Two-sided lighting: flip normal for back-facing vertices
    // A vertex is back-facing if the normal points away from the eye (dot(N, -V) < 0)
    if (uniforms.two_side_lighting && uniforms.lighting_enabled) {
        float3 V = normalize(-eye_pos.xyz);
        if (dot(eye_normal, V) < 0.0) {
            eye_normal = -eye_normal;
        }
    }
    out.eye_normal = eye_normal;

    // 4. Pass through texcoords (s, t, q) for projective texturing
    out.texcoord = in.texcoord;
    out.texcoord1 = in.texcoord1;
    // Pass through the secondary color (added after texturing, gated on GL_COLOR_SUM)
    out.secondary_color = in.secondary_color;

    // 5. Lighting or pass-through color
    if (uniforms.lighting_enabled) {
        // Initialize with emission + global_ambient * material.ambient
        float4 color = lighting.material.emission +
                       lighting.global_ambient * lighting.material.ambient;

        for (int i = 0; i < 8; i++) {
            if (lighting.lights[i].enabled == 0) continue;

            constant GLLight &light = lighting.lights[i];

            // Light direction and attenuation
            float3 L;
            float attenuation = 1.0;
            if (light.position.w == 0.0) {
                // Directional light
                L = normalize(light.position.xyz);
            } else {
                // Positional light
                float3 lightVec = light.position.xyz - eye_pos.xyz;
                float dist = length(lightVec);
                L = lightVec / max(dist, 0.0001);
                attenuation = 1.0 / (light.constant_atten +
                                     light.linear_atten * dist +
                                     light.quadratic_atten * dist * dist);
            }

            // Spotlight effect
            float spotEffect = 1.0;
            if (light.spot_cutoff >= 0.0) {
                // spot_cutoff stores cos(cutoff angle)
                float spotCos = dot(-L, normalize(light.spot_direction));
                if (spotCos < light.spot_cutoff) {
                    spotEffect = 0.0;
                } else {
                    spotEffect = pow(spotCos, light.spot_exponent);
                }
            }

            // Ambient contribution
            float4 ambient_contrib = light.ambient * lighting.material.ambient;

            // Diffuse contribution
            float NdotL = max(dot(eye_normal, L), 0.0);
            float4 diffuse_contrib = NdotL * light.diffuse * lighting.material.diffuse;

            // Specular contribution
            float4 specular_contrib = float4(0.0);
            if (NdotL > 0.0 && lighting.material.shininess > 0.0) {
                float3 V = normalize(-eye_pos.xyz);   // view direction (eye at origin)
                float3 H = normalize(L + V);           // half vector
                float NdotH = max(dot(eye_normal, H), 0.0);
                float spec = pow(NdotH, lighting.material.shininess);
                specular_contrib = spec * light.specular * lighting.material.specular;
            }

            color += attenuation * spotEffect *
                     (ambient_contrib + diffuse_contrib + specular_contrib);
        }

        out.color = clamp(color, 0.0, 1.0);
    } else {
        out.color = in.color;
    }

    // 6. Fog factor computation
    if (uniforms.fog_enabled) {
        float dist = abs(eye_pos.z);
        float fog_factor = 1.0;
        if (uniforms.fog_mode == 1) {
            // Linear
            fog_factor = (uniforms.fog_end - dist) / (uniforms.fog_end - uniforms.fog_start);
        } else if (uniforms.fog_mode == 2) {
            // Exp
            fog_factor = exp(-uniforms.fog_density * dist);
        } else if (uniforms.fog_mode == 3) {
            // Exp2
            float dz = uniforms.fog_density * dist;
            fog_factor = exp(-dz * dz);
        }
        out.fog_factor = clamp(fog_factor, 0.0, 1.0);
    } else {
        out.fog_factor = 1.0;
    }

    // 7. User clip plane distances (dot(plane, eye_pos) for each enabled plane)
    out.num_clip_planes = uniforms.num_clip_planes;
    out.clip_dist0 = (uniforms.num_clip_planes > 0) ? dot(uniforms.clip_planes[0], eye_pos) : 1.0;
    out.clip_dist1 = (uniforms.num_clip_planes > 1) ? dot(uniforms.clip_planes[1], eye_pos) : 1.0;
    out.clip_dist2 = (uniforms.num_clip_planes > 2) ? dot(uniforms.clip_planes[2], eye_pos) : 1.0;
    out.clip_dist3 = (uniforms.num_clip_planes > 3) ? dot(uniforms.clip_planes[3], eye_pos) : 1.0;
    out.clip_dist4 = (uniforms.num_clip_planes > 4) ? dot(uniforms.clip_planes[4], eye_pos) : 1.0;
    out.clip_dist5 = (uniforms.num_clip_planes > 5) ? dot(uniforms.clip_planes[5], eye_pos) : 1.0;

    return out;
}


// ---- Fragment shader ----

fragment float4 gl_fragment_main(
    GLVertexOut in [[stage_in]],
    constant GLFragmentUniforms &uniforms [[buffer(1)]],
    texture2d<float> tex [[texture(0)]],
    texture3d<float> tex3d [[texture(1)]],
    texture2d<float> tex1 [[texture(2)]],
    sampler samp [[sampler(0)]],
    sampler samp1 [[sampler(1)]])
{
    // User clip planes: discard if any clip distance is negative
    if (in.num_clip_planes > 0 && in.clip_dist0 < 0.0) discard_fragment();
    if (in.num_clip_planes > 1 && in.clip_dist1 < 0.0) discard_fragment();
    if (in.num_clip_planes > 2 && in.clip_dist2 < 0.0) discard_fragment();
    if (in.num_clip_planes > 3 && in.clip_dist3 < 0.0) discard_fragment();
    if (in.num_clip_planes > 4 && in.clip_dist4 < 0.0) discard_fragment();
    if (in.num_clip_planes > 5 && in.clip_dist5 < 0.0) discard_fragment();

    float4 color = in.color;

    // Texture application
    if (uniforms.has_texture) {
        // Projective texturing: divide s,t by q for perspective-correct mapping
        float2 uv = (in.texcoord.z != 0.0) ? in.texcoord.xy / in.texcoord.z : in.texcoord.xy;
        float4 tex_color;
        if (uniforms.has_texture_3d) {
            float3 uvw = float3(uv, in.texcoord1.x);  // use unit 1's s as the r (depth) coordinate
            tex_color = tex3d.sample(samp, uvw);
        } else {
            tex_color = tex.sample(samp, uv);
        }

        if (uniforms.texenv_mode == 0) {
            // GL_MODULATE: Cf = Ct * Cf
            color = float4(color.rgb * tex_color.rgb, color.a * tex_color.a);
        } else if (uniforms.texenv_mode == 1) {
            // GL_DECAL: Cf.rgb = mix(Cf.rgb, Ct.rgb, Ct.a), Cf.a unchanged
            color.rgb = mix(color.rgb, tex_color.rgb, tex_color.a);
        } else if (uniforms.texenv_mode == 2) {
            // GL_BLEND: Cf.rgb = mix(Cf.rgb, Cc.rgb, Ct.rgb), Cf.a = Cf.a * Ct.a
            color.rgb = mix(color.rgb, uniforms.texenv_color.rgb, tex_color.rgb);
            color.a *= tex_color.a;
        } else if (uniforms.texenv_mode == 3) {
            // GL_REPLACE: Cf = Ct
            color = tex_color;
        } else if (uniforms.texenv_mode == 4) {
            // GL_ADD: Cf.rgb = Cf.rgb + Ct.rgb, Cf.a = Cf.a * Ct.a
            color.rgb = clamp(color.rgb + tex_color.rgb, 0.0, 1.0);
            color.a *= tex_color.a;
        }
    }

    // ARB_multitexture unit 1 (glMultiTexCoord*ARB): sample the second 2D
    // texture with texcoord1 and combine it onto the running color per unit 1's texenv
    // mode (same modes 0-4 template as unit 0). Scope: 2-unit modulate/add (the standard
    // texenv modes). The GL_COMBINE crossbar is store-only and
    // GL_EXT_texture_env_combine is de-advertised — texenv1_mode is one of 0-4 only.
    if (uniforms.has_texture_unit1) {
        float2 uv1 = (in.texcoord1.z != 0.0) ? in.texcoord1.xy / in.texcoord1.z : in.texcoord1.xy;
        float4 t1 = tex1.sample(samp1, uv1);

        if (uniforms.texenv1_mode == 0) {
            // GL_MODULATE: Cf = Ct1 * Cf
            color = float4(color.rgb * t1.rgb, color.a * t1.a);
        } else if (uniforms.texenv1_mode == 1) {
            // GL_DECAL: Cf.rgb = mix(Cf.rgb, Ct1.rgb, Ct1.a), Cf.a unchanged
            color.rgb = mix(color.rgb, t1.rgb, t1.a);
        } else if (uniforms.texenv1_mode == 2) {
            // GL_BLEND: Cf.rgb = mix(Cf.rgb, Cc.rgb, Ct1.rgb), Cf.a = Cf.a * Ct1.a
            color.rgb = mix(color.rgb, uniforms.texenv_color.rgb, t1.rgb);
            color.a *= t1.a;
        } else if (uniforms.texenv1_mode == 3) {
            // GL_REPLACE: Cf = Ct1
            color = t1;
        } else if (uniforms.texenv1_mode == 4) {
            // GL_ADD: Cf.rgb = Cf.rgb + Ct1.rgb, Cf.a = Cf.a * Ct1.a
            color.rgb = clamp(color.rgb + t1.rgb, 0.0, 1.0);
            color.a *= t1.a;
        }
    }

    // Color sum (GL_COLOR_SUM / EXT_secondary_color): per OpenGL 1.2.1 §3.9 the
    // secondary (specular) color is added AFTER texturing and clamped, before fog.
    if (uniforms.color_sum_enabled) {
        color.rgb = saturate(color.rgb + in.secondary_color);
    }

    // Fog application
    if (uniforms.fog_color.w > -0.5) {
        // Use fog_color.w as a sentinel: >= 0 means fog is active
        // (We always compute fog_factor in vertex shader, check fog_color validity here)
        color.rgb = mix(uniforms.fog_color.rgb, color.rgb, in.fog_factor);
    }

    // Alpha test
    if (uniforms.alpha_test_enabled) {
        bool pass = true;
        float ref = uniforms.alpha_ref;
        if (uniforms.alpha_func == 0) {         // GL_NEVER
            pass = false;
        } else if (uniforms.alpha_func == 1) {  // GL_LESS
            pass = (color.a < ref);
        } else if (uniforms.alpha_func == 2) {  // GL_EQUAL
            pass = (color.a == ref);
        } else if (uniforms.alpha_func == 3) {  // GL_LEQUAL
            pass = (color.a <= ref);
        } else if (uniforms.alpha_func == 4) {  // GL_GREATER
            pass = (color.a > ref);
        } else if (uniforms.alpha_func == 5) {  // GL_NOTEQUAL
            pass = (color.a != ref);
        } else if (uniforms.alpha_func == 6) {  // GL_GEQUAL
            pass = (color.a >= ref);
        }
        // alpha_func == 7: GL_ALWAYS -> pass = true (default)
        if (!pass) {
            discard_fragment();
        }
    }

    if (uniforms.force_opaque_output) {
        color.a = 1.0;
    }

    return color;
}
