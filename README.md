# TD3D
Update to: https://github.com/AlbertTM8/TextureFusion with 3D capabilities.

TextureDiffusion3D
TextureDiffusion3D is a powerful Unreal Engine Editor plugin that bridges your 3D environment with the 2D generative capabilities of Stable Diffusion. By integrating directly with local or remote ComfyUI servers, this tool allows you to generate, project, and seamlessly blend high-quality AI textures (Base Color, Normals, Roughness, Metallic) onto your meshes without ever leaving the Unreal Editor.

Instead of manual UV unwrapping and external painting, TextureDiffusion3D uses a multi-camera projection mapping approach. It captures mesh data passes, sends them to your custom AI workflows, and intelligently re-projects the generated images back onto your model.

Key Features
Deep ComfyUI Integration: Submit depth maps, view-space normals, and edge masks to local or remote ComfyUI instances. The plugin automatically parses your JSON workflows and manages the upload/download pipeline.

Intelligent Auto-Rigging: Automatically generate optimal virtual camera placements around any mesh. The built-in algorithm ensures maximum surface coverage with the fewest possible cameras.

Advanced Projection & Blending: Handles the complex math of overlapping camera projections. Uses weighted blending based on surface normals and viewing angles, alongside high-pass filtering and tangent-space conversions to ensure seamless AI-generated materials.

UV Seam Handling: Built-in logic identifies and masks UV seams to prevent projection artifacts and texture drift across island boundaries.

Smart Performance Throttling: Automatically reduces Unreal Engine's rendering overhead (lowering screen percentage and VRAM streaming) while waiting for ComfyUI to process images, freeing up system resources for AI generation.

Youtube demo links:

[![Video Title](https://img.youtube.com/vi/nGa3zB0t-X8/maxresdefault.jpg)](https://youtu.be/nGa3zB0t-X8)

[![Video Title](https://img.youtube.com/vi/_joA2g-bzWY/maxresdefault.jpg)](https://youtu.be/_joA2g-bzWY)
