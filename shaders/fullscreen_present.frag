#version 450
layout(set=0,binding=0) uniform sampler2D imageA;
layout(set=0,binding=1) uniform sampler2D imageB;
layout(location=0) in vec2 screenUv;
layout(location=0) out vec4 outColor;
layout(push_constant) uniform PresentParameters {
    vec2 contentExtent;
    vec2 swapchainExtent;
    int srgbSwapchain;
    int comparison;
    float divider;
    int exactMapping;
    int presentationMode;
    int zoomEnabled;
    int finalFilter; // 0=nearest, 1=bilinear
    float zoom;
    vec2 zoomCenter;
    vec2 padding;
} p;
vec3 srgbToLinear(vec3 v){bvec3 c=lessThanEqual(v,vec3(0.04045));return mix(pow((v+0.055)/1.055,vec3(2.4)),v/12.92,c);}
vec4 fetchImage(bool useB,vec2 uv,bool exact){ivec2 q=clamp(ivec2(floor(uv*p.contentExtent)),ivec2(0),ivec2(p.contentExtent)-1);if(exact||p.finalFilter==0)return useB?texelFetch(imageB,q,0):texelFetch(imageA,q,0);return useB?texture(imageB,uv):texture(imageA,uv);}
void main(){float ca=p.contentExtent.x/p.contentExtent.y,sa=p.swapchainExtent.x/p.swapchainExtent.y;vec2 scale=vec2(1);
if(p.presentationMode==0)scale=p.contentExtent/p.swapchainExtent;
else if(p.presentationMode==1){if(sa>ca)scale.x=ca/sa;else scale.y=sa/ca;}
else if(p.presentationMode==2){if(sa>ca)scale.y=sa/ca;else scale.x=ca/sa;}
else{float k=min(p.swapchainExtent.x/p.contentExtent.x,p.swapchainExtent.y/p.contentExtent.y);if(k>=1)k=floor(k);scale=p.contentExtent*k/p.swapchainExtent;}
vec2 offset=(vec2(1)-scale)*0.5,uv=(screenUv-offset)/scale;if(any(lessThan(uv,vec2(0)))||any(greaterThanEqual(uv,vec2(1))))discard;
bool useB=p.comparison!=0&&uv.x>=p.divider;vec4 encoded=fetchImage(useB,uv,p.exactMapping!=0);
if(p.comparison!=0&&abs(uv.x-p.divider)<1.5/p.contentExtent.x)encoded=vec4(1,0.72,0.1,1);
if(p.zoomEnabled!=0){vec2 lo=vec2(0.70,0.04),hi=vec2(0.98,0.34);if(all(greaterThanEqual(screenUv,lo))&&all(lessThanEqual(screenUv,hi))){vec2 local=(screenUv-lo)/(hi-lo);bool zb=p.comparison!=0&&local.x>=0.5;vec2 zuv=p.zoomCenter+(local-0.5)/p.zoom;encoded=fetchImage(zb,zuv,true);if(any(lessThan(local,vec2(0.015)))||any(greaterThan(local,vec2(0.985))))encoded=vec4(1,1,1,1);}}
vec3 target=p.srgbSwapchain!=0?srgbToLinear(encoded.rgb):encoded.rgb;outColor=vec4(target,1);}