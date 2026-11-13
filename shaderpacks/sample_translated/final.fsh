#version 100
precision mediump float;

// Example "final.fsh" composite pass, Iris/OptiFine style.
// This is the kind of source real public Java-edition shaderpacks
// (BSL, SEUS, Complementary, etc.) ship. We translate THIS style of
// file, not Mojang's closed RenderDragon bytecode.

varying vec2 texcoord;

uniform sampler2D uSceneColor; // scene color
uniform sampler2D uSceneDepth; // scene depth
// (stubbed, not available on old renderer) uniform sampler2D shadowtex0; // shadow map (NOT available on old renderer)
uniform vec2 uTexelSize;
uniform float uTime;

vec3 tonemap(vec3 color) {
    color = color / (color + vec3(1.0));
    return pow(color, vec3(1.0 / 2.2));
}

void main() {
    vec3 color = texture2D(uSceneColor, texcoord).rgb;
    float depth = texture2D(uSceneDepth, texcoord).r;

    // fake bloom: blur a few taps
    vec3 bloom = vec3(0.0);
    for (int i = -2; i <= 2; i++) {
        bloom += texture2D(uSceneColor, texcoord + vec2(float(i) * uTexelSize.x, 0.0)).rgb;
    }
    bloom /= 5.0;
    color += bloom * 0.15;

    // vignette
    vec2 d = texcoord - 0.5;
    float vig = 1.0 - dot(d, d) * 0.6;
    color *= vig;

    // subtle pulsing warm tint using time
    color *= vec3(1.05, 1.0, 0.95) + 0.02 * sin(uTime);

    color = tonemap(color);
    gl_FragColor = vec4(color, 1.0);
}
