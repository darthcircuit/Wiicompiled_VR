// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from heurazy's mario-kart-wii-VR-port (GPL-3.0-or-later).
// Renders the VR cockpit overlay on a real GPU against cleared, occluding and
// partially occluding scene depth, forward and reversed, 1x and 4x MSAA.
#include "../lib/gfx/cockpit.hpp"
#include <fstream>
#include <iostream>
#include <atomic>
namespace aurora::webgpu { wgpu::Device g_device; wgpu::Queue g_queue; GraphicsConfig g_graphicsConfig{}; }
std::atomic<int> errors=0;
int main() {
  using namespace aurora;
  using namespace webgpu;
  wgpu::InstanceDescriptor id{};
  const wgpu::InstanceFeatureName timed=wgpu::InstanceFeatureName::TimedWaitAny;
  id.requiredFeatureCount=1;id.requiredFeatures=&timed;
  auto instance=wgpu::CreateInstance(&id);
  wgpu::Adapter adapter;
  wgpu::RequestAdapterOptions options{.backendType=wgpu::BackendType::D3D12};
  auto future=instance.RequestAdapter(&options,wgpu::CallbackMode::WaitAnyOnly,
    [&](wgpu::RequestAdapterStatus status,wgpu::Adapter a,wgpu::StringView message) {
      if(status==wgpu::RequestAdapterStatus::Success) adapter=std::move(a);
      else std::cerr<<std::string_view(message)<<'\n';
    });
  if(instance.WaitAny(future,5000000000)!=wgpu::WaitStatus::Success||!adapter) return 1;
  wgpu::DeviceDescriptor dd{};
  dd.SetUncapturedErrorCallback([](const wgpu::Device&,wgpu::ErrorType,wgpu::StringView message) {
    ++errors;std::cerr<<std::string_view(message)<<'\n';
  });
  future=adapter.RequestDevice(&dd,wgpu::CallbackMode::WaitAnyOnly,
    [&](wgpu::RequestDeviceStatus status,wgpu::Device device,wgpu::StringView message) {
      if(status==wgpu::RequestDeviceStatus::Success) g_device=std::move(device);
      else std::cerr<<std::string_view(message)<<'\n';
    });
  if(instance.WaitAny(future,5000000000)!=wgpu::WaitStatus::Success||!g_device) return 1;
  g_queue=g_device.GetQueue();
  g_graphicsConfig.surfaceConfiguration.format=wgpu::TextureFormat::RGBA8Unorm;
  g_graphicsConfig.depthFormat=wgpu::TextureFormat::Depth32Float;
  AuroraCockpit native{}; native.nativeWheel=true;
  if(!gfx::cockpit::geometry(native).empty()) return 1;
  native.nativeWheel=false;
  if(gfx::cockpit::geometry(native).empty()) return 1;
  for(bool bike : {false,true}) for(bool original : {false,true}) for(uint32_t samples : {1u,4u})
  for(bool hud : {false,true}) for(int coverage : {0,1,2}) for(bool reversed : {false,true}) for(uint32_t eyeIndex : {0u,1u}) {
    const bool occluded=coverage==1;
    gfx::StereoReplayFrame frame{};
    frame.cockpit.unitsPerMeter=100;
    frame.cockpit.active=true;frame.cockpit.wheelAngle=0.35f;
    frame.cockpit.nativeWheel=original;
    frame.cockpit.bike=bike;frame.cockpit.handlebarRadius=0.25f;
    const float handlePose[12]{1,0,0,0, 0,0,1,-0.3f, 0,-1,0,-0.42f};
    std::memcpy(frame.cockpit.seatFromHandlebar,handlePose,sizeof(handlePose));
    for(int hand=0;hand<2;++hand) {
      auto& h=frame.cockpit.hands[hand];h.tracked=true;h.held=true;h.squeeze=1;
      auto pose=gfx::cockpit::identity();pose[3]=hand?0.18f:-0.18f;pose[7]=-0.30f;pose[11]=-0.42f;
      std::memcpy(h.seatFromGrip,pose.data(),sizeof(h.seatFromGrip));
    }
    wgpu::TextureDescriptor td{.usage=wgpu::TextureUsage::RenderAttachment|wgpu::TextureUsage::CopySrc,
      .size={512,512,1},.format=wgpu::TextureFormat::RGBA8Unorm,.sampleCount=1};
    auto output=g_device.CreateTexture(&td);
    td.sampleCount=samples;td.usage=wgpu::TextureUsage::RenderAttachment;
    auto color=g_device.CreateTexture(&td);
    td.format=wgpu::TextureFormat::Depth24PlusStencil8;auto depth=g_device.CreateTexture(&td);
    auto& eye=frame.eyes[eyeIndex];eye.target.colorView=samples==1?output.CreateView():color.CreateView();
    if(samples>1) eye.target.resolveView=output.CreateView();
    eye.target.depthFormat=td.format;eye.target.depthView=depth.CreateView();eye.target.size={512,512,1};eye.target.msaaSamples=samples;
    eye.projection.m0[0]=1;eye.projection.m1[1]=1;
    eye.projection.m0[2]=eyeIndex?0.06f:-0.06f;
    auto view=gfx::cockpit::identity();view[7]=0.20f;
    std::memcpy(frame.cockpit.eyeFromSeat[eyeIndex],view.data(),sizeof(frame.cockpit.eyeFromSeat[eyeIndex]));
    auto encoder=g_device.CreateCommandEncoder();
    const wgpu::RenderPassColorAttachment clear{.view=eye.target.colorView,.resolveTarget=eye.target.resolveView,
      .loadOp=wgpu::LoadOp::Clear,.storeOp=wgpu::StoreOp::Store,.clearValue={0.06,0.09,0.13,1}};
    const wgpu::RenderPassDepthStencilAttachment sceneDepth{.view=eye.target.depthView,
      .depthLoadOp=wgpu::LoadOp::Clear,.depthStoreOp=wgpu::StoreOp::Store,.depthClearValue=reversed?(occluded?0.8f:0.0f):(occluded?0.2f:1.0f),
      .stencilLoadOp=wgpu::LoadOp::Clear,.stencilStoreOp=wgpu::StoreOp::Store,.stencilClearValue=0};
    const wgpu::RenderPassDescriptor pd{.colorAttachmentCount=1,.colorAttachments=&clear,.depthStencilAttachment=&sceneDepth};
    auto pass=encoder.BeginRenderPass(&pd);
    if(coverage==2) {
      wgpu::ShaderSourceWGSL code{};
      code.code=R"(
        @vertex fn vs(@builtin(vertex_index) i:u32) -> @builtin(position) vec4f {
          let p=array<vec2f,6>(vec2f(0,-1),vec2f(1,-1),vec2f(0,1),vec2f(0,1),vec2f(1,-1),vec2f(1,1));
          return vec4f(p[i],0.5,1);
        }
        @fragment fn fs() -> @location(0) vec4f { return vec4f(0.06,0.09,0.13,1); }
      )";
      wgpu::ShaderModuleDescriptor md{};md.nextInChain=&code;
      auto shader=g_device.CreateShaderModule(&md);
      const wgpu::ColorTargetState colorState{.format=wgpu::TextureFormat::RGBA8Unorm};
      const wgpu::FragmentState fragment{.module=shader,.entryPoint="fs",.targetCount=1,.targets=&colorState};
      const wgpu::DepthStencilState ds{.format=eye.target.depthFormat,.depthWriteEnabled=true,.depthCompare=wgpu::CompareFunction::Always};
      wgpu::RenderPipelineDescriptor desc{};desc.vertex={.module=shader,.entryPoint="vs"};
      desc.fragment=&fragment;desc.depthStencil=&ds;desc.multisample.count=samples;
      auto wall=g_device.CreateRenderPipeline(&desc);pass.SetPipeline(wall);pass.Draw(6);
    }
    if(coverage==2) gfx::cockpit::render(encoder,frame,eyeIndex,reversed?gfx::cockpit::SceneDepth{0,2,true}:gfx::cockpit::SceneDepth{-1,-2,true},&pass);
    pass.End();
    if(coverage!=2) gfx::cockpit::render(encoder,frame,eyeIndex,reversed?gfx::cockpit::SceneDepth{0,2,true}:gfx::cockpit::SceneDepth{-1,-2,true});
    if(hud) {
      // An opaque black screen and coloured HUD with depth testing disabled used
      // to overwrite the hands. Exercise a later pass too: the mask must survive.
      const wgpu::RenderPassColorAttachment load{.view=eye.target.colorView,.resolveTarget=eye.target.resolveView,
        .loadOp=wgpu::LoadOp::Load,.storeOp=wgpu::StoreOp::Store};
      const wgpu::RenderPassDepthStencilAttachment loadDepth{.view=eye.target.depthView,
        .depthLoadOp=wgpu::LoadOp::Load,.depthStoreOp=wgpu::StoreOp::Store,
        .stencilLoadOp=wgpu::LoadOp::Load,.stencilStoreOp=wgpu::StoreOp::Store};
      const wgpu::RenderPassDescriptor hudPass{.colorAttachmentCount=1,.colorAttachments=&load,.depthStencilAttachment=&loadDepth};
      auto overlay=encoder.BeginRenderPass(&hudPass);
      wgpu::ShaderSourceWGSL code{};
      code.code=R"(
        @vertex fn vs(@builtin(vertex_index) i:u32) -> @builtin(position) vec4f {
          let p=array<vec2f,3>(vec2f(-1,-1),vec2f(3,-1),vec2f(-1,3));
          return vec4f(p[i],0.5,1);
        }
        @fragment fn fs(@builtin(position) p:vec4f) -> @location(0) vec4f {
          return select(vec4f(0,0,0,1),vec4f(1,0,0,1),p.x<256);
        }
      )";
      wgpu::ShaderModuleDescriptor md{};md.nextInChain=&code;auto shader=g_device.CreateShaderModule(&md);
      const wgpu::ColorTargetState colorState{.format=wgpu::TextureFormat::RGBA8Unorm};
      const wgpu::FragmentState fragment{.module=shader,.entryPoint="fs",.targetCount=1,.targets=&colorState};
      const wgpu::StencilFaceState mask{.compare=wgpu::CompareFunction::Equal};
      const wgpu::DepthStencilState ds{.format=eye.target.depthFormat,.depthWriteEnabled=true,
        .depthCompare=wgpu::CompareFunction::Always,.stencilFront=mask,.stencilBack=mask,.stencilReadMask=1,.stencilWriteMask=0};
      wgpu::RenderPipelineDescriptor desc{};desc.vertex={.module=shader,.entryPoint="vs"};
      desc.fragment=&fragment;desc.depthStencil=&ds;desc.multisample.count=samples;
      auto screen=g_device.CreateRenderPipeline(&desc);overlay.SetPipeline(screen);overlay.Draw(3);overlay.End();
    }
    const wgpu::BufferDescriptor bd{.usage=wgpu::BufferUsage::CopyDst|wgpu::BufferUsage::MapRead,.size=512*512*4};
    auto readback=g_device.CreateBuffer(&bd);
    const wgpu::TexelCopyTextureInfo src{.texture=output};
    const wgpu::TexelCopyBufferInfo dst{.layout={.bytesPerRow=2048,.rowsPerImage=512},.buffer=readback};
    const wgpu::Extent3D extent{512,512,1};encoder.CopyTextureToBuffer(&src,&dst,&extent);
    auto commands=encoder.Finish();g_device.GetQueue().Submit(1,&commands);
    bool mapped=false;
    future=readback.MapAsync(wgpu::MapMode::Read,0,512*512*4,wgpu::CallbackMode::WaitAnyOnly,
      [&](wgpu::MapAsyncStatus status,wgpu::StringView) { mapped=status==wgpu::MapAsyncStatus::Success; });
    if(instance.WaitAny(future,5000000000)!=wgpu::WaitStatus::Success||!mapped) return 1;
    const auto* bytes=static_cast<const unsigned char*>(readback.GetConstMappedRange());
    size_t bright=0;
    for(size_t i=0;i<512*512;++i) if(bytes[4*i]>90&&bytes[4*i+1]>90&&bytes[4*i+2]>90) ++bright;
    if(hud && !(bytes[0]>250 && bytes[1]==0 && bytes[2]==0)) {
      std::cerr<<"HUD missing outside cockpit mask\n";++errors;
    }
    static size_t expectedBright[3][2][2]{};
    auto& expected=expectedBright[coverage][reversed][eyeIndex];
    if(!hud) expected=bright;
    else if(bright+64<expected || bright>expected+64) { std::cerr<<"HUD changed visible cockpit pixels\n";++errors; }
    if(occluded ? bright!=0 : bright<1000) { std::cerr<<"Incorrect hands/wheel occlusion\n";++errors; }
    if(coverage==2) {
      size_t left=0,right=0;
      for(size_t y=0;y<512;++y) for(size_t x=0;x<512;++x) {
        const auto i=y*512+x;
        if(bytes[4*i]>90&&bytes[4*i+1]>90&&bytes[4*i+2]>90) (x<256?left:right)++;
      }
      if(left<500||right>8) { std::cerr<<"Partial wall occlusion failed for eye "<<eyeIndex<<'\n';++errors; }
    }
    if(samples==4 && !original && !occluded) {
      std::ofstream image("cockpit-preview.ppm",std::ios::binary);image<<"P6\n512 512\n255\n";
      for(size_t i=0;i<512*512;++i) image.write(reinterpret_cast<const char*>(bytes+i*4),3);
    }
    readback.Unmap();
    std::cout<<(bike?"Bike ":"Kart ")<<(original?"native hands: ":"VR controls: ")<<samples<<"x MSAA: "<<bright<<" visible geometry pixels\n";
  }
  gfx::cockpit::shutdown();g_queue=nullptr;g_device.Destroy();g_device=nullptr;
  return errors?1:0;
}
