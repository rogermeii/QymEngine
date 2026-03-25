@echo off
echo Compiling Slang shaders to SPIR-V...

slangc Triangle.slang -target spirv -entry vertexMain -stage vertex -o vert.spv
slangc Triangle.slang -target spirv -entry fragmentMain -stage fragment -o frag.spv

slangc Lit.slang -target spirv -entry vertexMain -stage vertex -o lit_vert.spv
slangc Lit.slang -target spirv -entry fragmentMain -stage fragment -o lit_frag.spv

slangc Unlit.slang -target spirv -entry vertexMain -stage vertex -o unlit_vert.spv
slangc Unlit.slang -target spirv -entry fragmentMain -stage fragment -o unlit_frag.spv

slangc Grid.slang -target spirv -entry vertexMain -stage vertex -o grid_vert.spv
slangc Grid.slang -target spirv -entry fragmentMain -stage fragment -o grid_frag.spv

echo Done.
pause
