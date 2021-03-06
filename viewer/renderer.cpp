#include "renderer.h"

#include "scene.h"

#include "preamble.glsl"

#include "imgui.h"

#include <glm/gtx/transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cstdio>
#include <memory>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

class Renderer : public IRenderer
{
public:
    const int kSampleCount = 4;
    const int kMaxTextureCount = 32;

    struct GPUTimestamps
    {
        enum Enum
        {
            RenderSceneStart,
            RenderSceneEnd,
            MultisampleResolveStart,
            MultisampleResolveEnd,
            ReadbackBackbufferStart,
            ReadbackBackbufferEnd,
            ComputeSATStart,
            ComputeSATEnd,
            SATUploadStart,
            SATUploadEnd,
            DOFBlurStart,
            DOFBlurEnd,
            RenderGUIStart,
            RenderGUIEnd,
            BlitToWindowStart,
            BlitToWindowEnd,
            Count
        };

        static constexpr const char* Names[Count / 2] = {
            "RenderScene",
            "MultisampleResolve",
            "ReadbackBackbuffer",
            "ComputeSAT",
            "SATUpload",
            "DOfBlur",
            "RenderGUI",
            "BlitToWindow"
        };
    };

    struct CPUTimestamps
    {
        enum Enum
        {
            ReadbackBackbufferStart,
            ReadbackBackbufferEnd,
            ComputeSATStart,
            ComputeSATEnd,
            SATUploadStart,
            SATUploadEnd,
            Count
        };

        static constexpr const char* Names[Count / 2] = {
            "ReadbackBackbuffer",
            "ComputeSAT",
            "SATUpload"
        };
    };

    Scene* mScene;

    bool mFirstFrame;

    ShaderSet mShaders;
    GLuint* mSceneSP;

    int mBackbufferWidth;
    int mBackbufferHeight;
    // multi-sampled buffers
    GLuint mBackbufferFBOMS;
    GLuint mBackbufferColorTOMS;
    GLuint mBackbufferDepthTOMS;
    // single-sampled buffers
    GLuint mBackbufferFBOSS;
    GLuint mBackbufferColorTOSS;
    GLuint mBackbufferDepthTOSS;

    // empty VAO, for attrib-less rendering passes
    GLuint mNullVAO;

    // may be different from backbuffer if scaled
    int mWindowWidth;
    int mWindowHeight;

    int mSummedAreaTableWidth;
    int mSummedAreaTableHeight;
    bool mUseCPUForSAT;
    glm::u8vec4* mCPUBackbufferReadback;
    glm::uvec4* mCPUSummedAreaTable;
    GLuint* mSummedAreaTableUpsweepSP;
    GLuint* mSummedAreaTableDownsweepSP;
    GLuint* mTransposeSummedAreaTableSP;
    GLuint mSummedRowsTO, *mSummedAreaTableTO; // aliases
    GLuint mSummedRowsWGSumsTO;
    GLuint mSummedColsTO;
    GLuint mSummedColsWGSumsTO;

    bool mEnableDoF;
    GLuint* mDepthOfFieldSP;
    float mFocusDepth;

    GLuint mGPUTimestampQueries[GPUTimestamps::Count];
    GLuint64 mGPUTimestampQueryResults[GPUTimestamps::Count];
    LARGE_INTEGER mCPUTimestampQueryResults[CPUTimestamps::Count];

    void Init(Scene* scene) override
    {
        mScene = scene;

        mFirstFrame = true;

        mShaders.SetVersion("440");
        mShaders.SetPreambleFile("preamble.glsl");

        mSceneSP = mShaders.AddProgramFromExts({ "scene.vert", "scene.frag" });
        mSummedAreaTableUpsweepSP = mShaders.AddProgramFromExts({ "sat_up.comp" });
        mSummedAreaTableDownsweepSP = mShaders.AddProgramFromExts({ "sat_down.comp" });
        mTransposeSummedAreaTableSP = mShaders.AddProgramFromExts({ "sat_transpose.comp" });
        mDepthOfFieldSP = mShaders.AddProgramFromExts({ "blit.vert", "dof.frag" });

        glGenVertexArrays(1, &mNullVAO);
        glBindVertexArray(mNullVAO);
        glBindVertexArray(0);

        mEnableDoF = true;
        mFocusDepth = 5.0f;

        glGenQueries(GPUTimestamps::Count, &mGPUTimestampQueries[0]);
    }

    void Resize(int width, int height) override
    {
        mWindowWidth = width;
        mWindowHeight = height;

        mBackbufferWidth = mWindowWidth;
        mBackbufferHeight = mWindowHeight;

        // OS X doesn't like it when you delete framebuffers it's using
        // No big deal, this happens implicitly anyways.
        glFinish();

        // Init multisampled FBO
        {
            glDeleteTextures(1, &mBackbufferColorTOMS);
            glGenTextures(1, &mBackbufferColorTOMS);
            glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, mBackbufferColorTOMS);
            glTexStorage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, kSampleCount, GL_SRGB8_ALPHA8, mBackbufferWidth, mBackbufferHeight, GL_TRUE);
            glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);

            glDeleteTextures(1, &mBackbufferDepthTOMS);
            glGenTextures(1, &mBackbufferDepthTOMS);
            glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, mBackbufferDepthTOMS);
            glTexStorage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, kSampleCount, GL_DEPTH_COMPONENT32F, mBackbufferWidth, mBackbufferHeight, GL_TRUE);
            glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);

            glDeleteFramebuffers(1, &mBackbufferFBOMS);
            glGenFramebuffers(1, &mBackbufferFBOMS);
            glBindFramebuffer(GL_FRAMEBUFFER, mBackbufferFBOMS);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, mBackbufferColorTOMS, 0);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D_MULTISAMPLE, mBackbufferDepthTOMS, 0);
            GLenum fboStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (fboStatus != GL_FRAMEBUFFER_COMPLETE) {
                fprintf(stderr, "glCheckFramebufferStatus: %x\n", fboStatus);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }

        // Init singlesampled FBO
        {
            glDeleteTextures(1, &mBackbufferColorTOSS);
            glGenTextures(1, &mBackbufferColorTOSS);
            glBindTexture(GL_TEXTURE_2D, mBackbufferColorTOSS);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_SRGB8_ALPHA8, mBackbufferWidth, mBackbufferHeight);
            glBindTexture(GL_TEXTURE_2D, 0);

            glDeleteTextures(1, &mBackbufferDepthTOSS);
            glGenTextures(1, &mBackbufferDepthTOSS);
            glBindTexture(GL_TEXTURE_2D, mBackbufferDepthTOSS);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT32F, mBackbufferWidth, mBackbufferHeight);
            glBindTexture(GL_TEXTURE_2D, 0);

            glDeleteFramebuffers(1, &mBackbufferFBOSS);
            glGenFramebuffers(1, &mBackbufferFBOSS);
            glBindFramebuffer(GL_FRAMEBUFFER, mBackbufferFBOSS);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mBackbufferColorTOSS, 0);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, mBackbufferDepthTOSS, 0);
            GLenum fboStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (fboStatus != GL_FRAMEBUFFER_COMPLETE) {
                fprintf(stderr, "glCheckFramebufferStatus: %x\n", fboStatus);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }

        // Init summed area table
        {
            mSummedAreaTableWidth = (mBackbufferWidth + SAT_WORKGROUP_SIZE_X - 1) & -SAT_WORKGROUP_SIZE_X;
            mSummedAreaTableHeight = (mBackbufferHeight + SAT_WORKGROUP_SIZE_X - 1) & -SAT_WORKGROUP_SIZE_X;

            // The workgroup sums need to be computed in a single workgroup
            // could generalize this to an arbitrary number of iterations, but meh.
            assert(mSummedAreaTableWidth / SAT_WORKGROUP_SIZE_X <= SAT_WORKGROUP_SIZE_X);
            assert(mSummedAreaTableHeight / SAT_WORKGROUP_SIZE_X <= SAT_WORKGROUP_SIZE_X);

            delete[] mCPUBackbufferReadback;
            mCPUBackbufferReadback = new glm::u8vec4[mBackbufferWidth * mBackbufferHeight];

            delete[] mCPUSummedAreaTable;
            mCPUSummedAreaTable = new glm::uvec4[mSummedAreaTableWidth * mSummedAreaTableHeight];

            glDeleteTextures(1, &mSummedRowsTO);
            glGenTextures(1, &mSummedRowsTO);
            glBindTexture(GL_TEXTURE_2D, mSummedRowsTO);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32UI, mSummedAreaTableWidth, mSummedAreaTableHeight);
            glBindTexture(GL_TEXTURE_2D, 0);

            mSummedAreaTableTO = &mSummedRowsTO;

            glDeleteTextures(1, &mSummedRowsWGSumsTO);
            glGenTextures(1, &mSummedRowsWGSumsTO);
            glBindTexture(GL_TEXTURE_2D, mSummedRowsWGSumsTO);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32UI, mSummedAreaTableWidth / SAT_WORKGROUP_SIZE_X, mSummedAreaTableHeight);
            glBindTexture(GL_TEXTURE_2D, 0);

            glDeleteTextures(1, &mSummedColsTO);
            glGenTextures(1, &mSummedColsTO);
            glBindTexture(GL_TEXTURE_2D, mSummedColsTO);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32UI, mSummedAreaTableHeight, mSummedAreaTableWidth);
            glBindTexture(GL_TEXTURE_2D, 0);

            glDeleteTextures(1, &mSummedColsWGSumsTO);
            glGenTextures(1, &mSummedColsWGSumsTO);
            glBindTexture(GL_TEXTURE_2D, mSummedColsWGSumsTO);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32UI, mSummedAreaTableHeight / SAT_WORKGROUP_SIZE_X, mSummedAreaTableWidth);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
    }

    void UpdateGUI()
    {
        // Readback last frame's timestamps and display them
        if (ImGui::Begin("Renderer Profiling") && !mFirstFrame)
        {
            ImGui::Text("GPU time");
            for (int i = 0; i < GPUTimestamps::Count / 2; i++)
            {
                if (!mUseCPUForSAT)
                {
                    if (i * 2 == GPUTimestamps::ReadbackBackbufferStart ||
                        i * 2 == GPUTimestamps::SATUploadStart)
                    {
                        continue;
                    }
                }
                else
                {
                    if (i * 2 == GPUTimestamps::ComputeSATStart)
                    {
                        continue;
                    }
                }

                glGetQueryObjectui64v(mGPUTimestampQueries[i * 2 + 0], GL_QUERY_RESULT, &mGPUTimestampQueryResults[i * 2 + 0]);
                glGetQueryObjectui64v(mGPUTimestampQueries[i * 2 + 1], GL_QUERY_RESULT, &mGPUTimestampQueryResults[i * 2 + 1]);

                uint64_t ns = mGPUTimestampQueryResults[i * 2 + 1] - mGPUTimestampQueryResults[i * 2 + 0];
                uint64_t ms = ns / 1000000;
                ImGui::Text("%s: %d.%d milliseconds", GPUTimestamps::Names[i], ms, ns / 1000 - ms * 1000);
            }

            ImGui::Text("\nCPU time");
            
            LARGE_INTEGER freq;
            QueryPerformanceFrequency(&freq);

            for (int i = 0; i < CPUTimestamps::Count / 2; i++)
            {
                if (!mUseCPUForSAT)
                {
                    if (i * 2 == CPUTimestamps::ReadbackBackbufferStart ||
                        i * 2 == CPUTimestamps::ComputeSATStart ||
                        i * 2 == CPUTimestamps::SATUploadStart)
                    {
                        continue;
                    }
                }

                uint64_t ticks = mCPUTimestampQueryResults[i * 2 + 1].QuadPart - mCPUTimestampQueryResults[i * 2 + 0].QuadPart;
                uint64_t us = ticks * 1000000 / freq.QuadPart;
                uint64_t ms = us / 1000;
                ImGui::Text("%s: %d.%d milliseconds", CPUTimestamps::Names[i], ms, us - ms * 1000);
            }
        }
        ImGui::End();

        if (ImGui::Begin("Renderer"))
        {
            ImGui::Checkbox("Enable DoF", &mEnableDoF);
            ImGui::Checkbox("CPU SAT", &mUseCPUForSAT);
            ImGui::SliderFloat("Focus Depth", &mFocusDepth, 0.0f, 10.0f);
        }
        ImGui::End();
    }

    void Paint() override
    {
        UpdateGUI();

        // Reload any programs
        mShaders.UpdatePrograms();

        // Render scene
        glQueryCounter(mGPUTimestampQueries[GPUTimestamps::RenderSceneStart], GL_TIMESTAMP);
        if (*mSceneSP)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, mBackbufferFBOMS);
            glViewport(0, 0, mBackbufferWidth, mBackbufferHeight);

            glClearColor(100.0f / 255.0f, 149.0f / 255.0f, 237.0f / 255.0f, 1.0f);
            glClearDepth(0.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            Camera& mainCamera = mScene->Cameras[mScene->MainCameraID];

            glm::vec3 eye = mainCamera.Eye;
            glm::vec3 up = mainCamera.Up;

            glm::mat4 V = glm::lookAt(eye, mainCamera.Target, up);
            glm::mat4 P;
            {
                float f = 1.0f / tanf(mainCamera.FovY / 2.0f);
                P = glm::mat4(
                    f / mainCamera.Aspect, 0.0f, 0.0f, 0.0f,
                    0.0f, f, 0.0f, 0.0f,
                    0.0f, 0.0f, 0.0f, -1.0f,
                    0.0f, 0.0f, mainCamera.ZNear, 0.0f);
            }

            glm::mat4 VP = P * V;

            glUseProgram(*mSceneSP);

            glUniform3fv(SCENE_CAMERAPOS_UNIFORM_LOCATION, 1, value_ptr(eye));

            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_GREATER);
            glEnable(GL_FRAMEBUFFER_SRGB);
            for (uint32_t instanceID : mScene->Instances)
            {
                const Instance* instance = &mScene->Instances[instanceID];
                const Mesh* mesh = &mScene->Meshes[instance->MeshID];
                const Transform* transform = &mScene->Transforms[instance->TransformID];

                glm::mat4 MW;
                MW = translate(-transform->RotationOrigin) * MW;
                MW = mat4_cast(transform->Rotation) * MW;
                MW = translate(transform->RotationOrigin) * MW;
                MW = scale(transform->Scale) * MW;
                MW = translate(transform->Translation) * MW;

                glm::mat3 N_MW;
                N_MW = mat3_cast(transform->Rotation) * N_MW;
                N_MW = glm::mat3(scale(1.0f / transform->Scale)) * N_MW;

                glm::mat4 MVP = VP * MW;

                glUniformMatrix4fv(SCENE_MW_UNIFORM_LOCATION, 1, GL_FALSE, value_ptr(MW));
                glUniformMatrix3fv(SCENE_N_MW_UNIFORM_LOCATION, 1, GL_FALSE, value_ptr(N_MW));
                glUniformMatrix4fv(SCENE_MVP_UNIFORM_LOCATION, 1, GL_FALSE, value_ptr(MVP));

                glBindVertexArray(mesh->MeshVAO);
                for (size_t meshDrawIdx = 0; meshDrawIdx < mesh->DrawCommands.size(); meshDrawIdx++)
                {
                    const GLDrawElementsIndirectCommand* drawCmd = &mesh->DrawCommands[meshDrawIdx];
                    const Material* material = &mScene->Materials[mesh->MaterialIDs[meshDrawIdx]];

                    glActiveTexture(GL_TEXTURE0 + SCENE_DIFFUSE_MAP_TEXTURE_BINDING);
                    if (material->DiffuseMapID == -1)
                    {
                        glBindTexture(GL_TEXTURE_2D, 0);
                        glUniform1i(SCENE_HAS_DIFFUSE_MAP_UNIFORM_LOCATION, 0);
                    }
                    else
                    {
                        const DiffuseMap* diffuseMap = &mScene->DiffuseMaps[material->DiffuseMapID];
                        glBindTexture(GL_TEXTURE_2D, diffuseMap->DiffuseMapTO);
                        glUniform1i(SCENE_HAS_DIFFUSE_MAP_UNIFORM_LOCATION, 1);
                    }

                    glUniform3fv(SCENE_AMBIENT_UNIFORM_LOCATION, 1, material->Ambient);
                    glUniform3fv(SCENE_DIFFUSE_UNIFORM_LOCATION, 1, material->Diffuse);
                    glUniform3fv(SCENE_SPECULAR_UNIFORM_LOCATION, 1, material->Specular);
                    glUniform1f(SCENE_SHININESS_UNIFORM_LOCATION, material->Shininess);

                    glDrawElementsInstancedBaseVertexBaseInstance(
                        GL_TRIANGLES,
                        drawCmd->count,
                        GL_UNSIGNED_INT, (GLvoid*)(sizeof(uint32_t) * drawCmd->firstIndex),
                        drawCmd->primCount,
                        drawCmd->baseVertex,
                        drawCmd->baseInstance);
                }
                glBindVertexArray(0);
            }
            glBindTextures(0, kMaxTextureCount, NULL);
            glDisable(GL_FRAMEBUFFER_SRGB);
            glDepthFunc(GL_LESS);
            glDisable(GL_DEPTH_TEST);
            glUseProgram(0);

            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        glQueryCounter(mGPUTimestampQueries[GPUTimestamps::RenderSceneEnd], GL_TIMESTAMP);

        // resolve multisampled backbuffer to singlesampled backbuffer
        glQueryCounter(mGPUTimestampQueries[GPUTimestamps::MultisampleResolveStart], GL_TIMESTAMP);
        {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, mBackbufferFBOMS);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mBackbufferFBOSS);
            glBlitFramebuffer(
                0, 0, mBackbufferWidth, mBackbufferHeight,
                0, 0, mBackbufferWidth, mBackbufferHeight,
                GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        glQueryCounter(mGPUTimestampQueries[GPUTimestamps::MultisampleResolveEnd], GL_TIMESTAMP);

        if (mEnableDoF)
        {
            // Compute SAT for the rendered image
            if (mUseCPUForSAT)
            {
                // Dumb CPU SAT. Mainly used as a reference.

                // Readback backbuffer to SAT-ify it
                QueryPerformanceCounter(&mCPUTimestampQueryResults[CPUTimestamps::ReadbackBackbufferStart]);
                glQueryCounter(mGPUTimestampQueries[GPUTimestamps::ReadbackBackbufferStart], GL_TIMESTAMP);
                {
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, mBackbufferFBOSS);
                    glReadPixels(0, 0, mBackbufferWidth, mBackbufferHeight, GL_RGBA, GL_UNSIGNED_BYTE, &mCPUBackbufferReadback[0]);
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                }
                glQueryCounter(mGPUTimestampQueries[GPUTimestamps::ReadbackBackbufferEnd], GL_TIMESTAMP);
                QueryPerformanceCounter(&mCPUTimestampQueryResults[CPUTimestamps::ReadbackBackbufferEnd]);

                QueryPerformanceCounter(&mCPUTimestampQueryResults[CPUTimestamps::ComputeSATStart]);
                
                // sum the rows
                for (int row = 0; row < mBackbufferHeight; row++)
                {
                    glm::uvec4 first = glm::uvec4(mCPUBackbufferReadback[row * mBackbufferWidth + 0]);
                    first = glm::uvec4(pow(glm::vec4(first) / 255.0f, glm::vec4(2.2f)) * 255.0f);
                    mCPUSummedAreaTable[row * mSummedAreaTableWidth + 0] = first;

                    for (int col = 1; col < mBackbufferWidth; col++)
                    {
                        glm::uvec4 readback = glm::uvec4(mCPUBackbufferReadback[row * mBackbufferWidth + col]);
                        readback = glm::uvec4(pow(glm::vec4(readback) / 255.0f, glm::vec4(2.2f)) * 255.0f);
                        mCPUSummedAreaTable[row * mSummedAreaTableWidth + col] = readback + mCPUSummedAreaTable[row * mSummedAreaTableWidth + (col - 1)];
                    }
                }

                // sum the columns (gross memory access...)
                for (int col = 0; col < mBackbufferWidth; col++)
                {
                    for (int row = 1; row < mBackbufferHeight; row++)
                    {
                        mCPUSummedAreaTable[row * mSummedAreaTableWidth + col] += mCPUSummedAreaTable[(row - 1) * mSummedAreaTableWidth + col];
                    }
                }
                QueryPerformanceCounter(&mCPUTimestampQueryResults[CPUTimestamps::ComputeSATEnd]);

                // Upload SAT back to GPU
                QueryPerformanceCounter(&mCPUTimestampQueryResults[CPUTimestamps::SATUploadStart]);
                glQueryCounter(mGPUTimestampQueries[GPUTimestamps::SATUploadStart], GL_TIMESTAMP);
                {
                    glBindTexture(GL_TEXTURE_2D, *mSummedAreaTableTO);
                    for (int row = 0; row < mBackbufferHeight; row++)
                    {
                        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, mBackbufferWidth, 1, GL_RGBA_INTEGER, GL_UNSIGNED_INT, &mCPUSummedAreaTable[row * mSummedAreaTableWidth]);
                    }
                    glBindTexture(GL_TEXTURE_2D, 0);
                }
                glQueryCounter(mGPUTimestampQueries[GPUTimestamps::SATUploadEnd], GL_TIMESTAMP);
                QueryPerformanceCounter(&mCPUTimestampQueryResults[CPUTimestamps::SATUploadEnd]);
            }
            else
            {
                // GPU SAT
                glQueryCounter(mGPUTimestampQueries[GPUTimestamps::ComputeSATStart], GL_TIMESTAMP);
                if (*mSummedAreaTableUpsweepSP && *mSummedAreaTableDownsweepSP && *mTransposeSummedAreaTableSP)
                {
                    enum SATPass {
                        SATPass_Rows,
                        SATPass_Cols,
                        SATPass_Count
                    };

                    for (int pass = 0; pass < SATPass_Count; pass++)
                    {
                        // Up-sweep
                        {
                            glUseProgram(*mSummedAreaTableUpsweepSP);

                            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

                            if (pass == SATPass_Rows) {
                                glBindTextures(SAT_INPUT_TEXTURE_BINDING, 1, &mBackbufferColorTOSS);
                                glBindImageTexture(SAT_OUTPUT_IMAGE_BINDING, mSummedRowsTO, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA32UI);
                                glUniform1i(SAT_READ_UINT_INPUT_UNIFORM_LOCATION, 0);
                            }
                            else if (pass == SATPass_Cols) {
                                glBindTextures(SAT_UINT_INPUT_TEXTURE_BINDING, 1, &mSummedColsTO);
                                glBindImageTexture(SAT_OUTPUT_IMAGE_BINDING, mSummedColsTO, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA32UI);
                                glUniform1i(SAT_READ_UINT_INPUT_UNIFORM_LOCATION, 1);
                            }

                            glUniform1i(SAT_READ_WGSUM_UNIFORM_LOCATION, 0);

                            if (pass == SATPass_Rows) {
                                glDispatchCompute(mSummedAreaTableWidth / SAT_WORKGROUP_SIZE_X, mBackbufferHeight, 1);
                            }
                            else if (pass == SATPass_Cols) {
                                glDispatchCompute(mSummedAreaTableHeight / SAT_WORKGROUP_SIZE_X, mBackbufferWidth, 1);
                            }

                            glBindTextures(SAT_INPUT_TEXTURE_BINDING, 1, NULL);
                            glBindImageTextures(SAT_OUTPUT_IMAGE_BINDING, 1, NULL);
                            glUseProgram(0);
                        }

                        // Up-sweep WG sums
                        {
                            glUseProgram(*mSummedAreaTableUpsweepSP);

                            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

                            if (pass == SATPass_Rows) {
                                glBindTextures(SAT_UINT_INPUT_TEXTURE_BINDING, 1, &mSummedRowsTO);
                                glBindImageTexture(SAT_OUTPUT_IMAGE_BINDING, mSummedRowsWGSumsTO, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA32UI);
                            }
                            else if (pass == SATPass_Cols) {
                                glBindTextures(SAT_UINT_INPUT_TEXTURE_BINDING, 1, &mSummedColsTO);
                                glBindImageTexture(SAT_OUTPUT_IMAGE_BINDING, mSummedColsWGSumsTO, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA32UI);
                            }

                            glUniform1i(SAT_READ_UINT_INPUT_UNIFORM_LOCATION, 0);
                            glUniform1i(SAT_READ_WGSUM_UNIFORM_LOCATION, 1);

                            if (pass == SATPass_Rows) {
                                glDispatchCompute(1, mBackbufferHeight, 1);
                            }
                            else if (pass == SATPass_Cols) {
                                glDispatchCompute(1, mBackbufferWidth, 1);
                            }

                            glBindTextures(SAT_UINT_INPUT_TEXTURE_BINDING, 1, NULL);
                            glBindImageTextures(SAT_OUTPUT_IMAGE_BINDING, 1, NULL);
                            glUseProgram(0);
                        }

                        // Down-sweep WG sums
                        {
                            glUseProgram(*mSummedAreaTableDownsweepSP);

                            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

                            if (pass == SATPass_Rows) {
                                glBindImageTexture(SAT_OUTPUT_IMAGE_BINDING, mSummedRowsWGSumsTO, 0, GL_TRUE, 0, GL_READ_WRITE, GL_RGBA32UI);
                            }
                            else if (pass == SATPass_Cols) {
                                glBindImageTexture(SAT_OUTPUT_IMAGE_BINDING, mSummedColsWGSumsTO, 0, GL_TRUE, 0, GL_READ_WRITE, GL_RGBA32UI);
                            }

                            glUniform1i(SAT_ADD_WGSUM_UNIFORM_LOCATION, 0);

                            if (pass == SATPass_Rows) {
                                glDispatchCompute(1, mBackbufferHeight, 1);
                            }
                            else if (pass == SATPass_Cols) {
                                glDispatchCompute(1, mBackbufferWidth, 1);
                            }

                            glBindImageTextures(SAT_OUTPUT_IMAGE_BINDING, 1, NULL);
                            glUseProgram(0);
                        }

                        // Down-sweep
                        {
                            glUseProgram(*mSummedAreaTableDownsweepSP);

                            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

                            if (pass == SATPass_Rows) {
                                glBindImageTexture(SAT_OUTPUT_IMAGE_BINDING, mSummedRowsTO, 0, GL_TRUE, 0, GL_READ_WRITE, GL_RGBA32UI);
                                glBindImageTexture(SAT_WGSUMS_IMAGE_BINDING, mSummedRowsWGSumsTO, 0, GL_TRUE, 0, GL_READ_ONLY, GL_RGBA32UI);
                            }
                            else if (pass == SATPass_Cols) {
                                glBindImageTexture(SAT_OUTPUT_IMAGE_BINDING, mSummedColsTO, 0, GL_TRUE, 0, GL_READ_WRITE, GL_RGBA32UI);
                                glBindImageTexture(SAT_WGSUMS_IMAGE_BINDING, mSummedColsWGSumsTO, 0, GL_TRUE, 0, GL_READ_ONLY, GL_RGBA32UI);
                            }

                            glUniform1i(SAT_ADD_WGSUM_UNIFORM_LOCATION, 1);

                            if (pass == SATPass_Rows) {
                                glDispatchCompute(mSummedAreaTableWidth / SAT_WORKGROUP_SIZE_X, mBackbufferHeight, 1);
                            }
                            else if (pass == SATPass_Cols) {
                                glDispatchCompute(mSummedAreaTableHeight / SAT_WORKGROUP_SIZE_X, mBackbufferWidth, 1);
                            }

                            glBindImageTextures(SAT_OUTPUT_IMAGE_BINDING, 1, NULL);
                            glBindImageTextures(SAT_WGSUMS_IMAGE_BINDING, 1, NULL);
                            glUseProgram(0);
                        }

                        // Transpose
                        {
                            glUseProgram(*mTransposeSummedAreaTableSP);

                            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

                            if (pass == SATPass_Rows) {
                                glBindImageTexture(TRANSPOSE_SAT_INPUT_IMAGE_BINDING, mSummedRowsTO, 0, GL_TRUE, 0, GL_READ_ONLY, GL_RGBA32UI);
                                glBindImageTexture(TRANSPOSE_SAT_OUTPUT_IMAGE_BINDING, mSummedColsTO, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA32UI);
                            }
                            else if (pass == SATPass_Cols) {
                                glBindImageTexture(TRANSPOSE_SAT_INPUT_IMAGE_BINDING, mSummedColsTO, 0, GL_TRUE, 0, GL_READ_ONLY, GL_RGBA32UI);
                                glBindImageTexture(TRANSPOSE_SAT_OUTPUT_IMAGE_BINDING, *mSummedAreaTableTO, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA32UI);
                            }

                            if (pass == SATPass_Rows) {
                                glDispatchCompute(mSummedAreaTableWidth / TRANSPOSE_SAT_WORKGROUP_SIZE_X, mSummedAreaTableHeight / TRANSPOSE_SAT_WORKGROUP_SIZE_X, 1);
                            }
                            else if (pass == SATPass_Cols) {
                                glDispatchCompute(mSummedAreaTableHeight / TRANSPOSE_SAT_WORKGROUP_SIZE_X, mSummedAreaTableWidth / TRANSPOSE_SAT_WORKGROUP_SIZE_X, 1);
                            }

                            glBindImageTextures(TRANSPOSE_SAT_INPUT_IMAGE_BINDING, 1, NULL);
                            glBindImageTextures(TRANSPOSE_SAT_OUTPUT_IMAGE_BINDING, 1, NULL);
                            glUseProgram(0);
                        }
                    }
                }
                glQueryCounter(mGPUTimestampQueries[GPUTimestamps::ComputeSATEnd], GL_TIMESTAMP);
            }

            // Apply DoF-blur to scene
            glQueryCounter(mGPUTimestampQueries[GPUTimestamps::DOFBlurStart], GL_TIMESTAMP);
            if (*mDepthOfFieldSP)
            {
                // ensure the computed SAT is available to the DoF shader
                glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);

                glBindFramebuffer(GL_FRAMEBUFFER, mBackbufferFBOSS);
                glUseProgram(*mDepthOfFieldSP);
                glBindVertexArray(mNullVAO);
                glBindTextures(DOF_SAT_TEXTURE_BINDING, 1, &*mSummedAreaTableTO);
                glBindTextures(DOF_DEPTH_TEXTURE_BINDING, 1, &mBackbufferDepthTOSS);
                glEnable(GL_FRAMEBUFFER_SRGB);

                Camera& mainCamera = mScene->Cameras[mScene->MainCameraID];

                glUniform1f(DOF_ZNEAR_UNIFORM_LOCATION, mainCamera.ZNear);
                glUniform1f(DOF_FOCUS_UNIFORM_LOCATION, mFocusDepth);
            
                glDrawArrays(GL_TRIANGLES, 0, 3);
            
                glDisable(GL_FRAMEBUFFER_SRGB);
                glBindTextures(DOF_SAT_TEXTURE_BINDING, 1, NULL);
                glBindTextures(DOF_DEPTH_TEXTURE_BINDING, 1, NULL);
                glBindVertexArray(0);
                glUseProgram(0);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
            }
            glQueryCounter(mGPUTimestampQueries[GPUTimestamps::DOFBlurEnd], GL_TIMESTAMP);
        
        } // endif enable DOF

        // Render GUI
        glQueryCounter(mGPUTimestampQueries[GPUTimestamps::RenderGUIStart], GL_TIMESTAMP);
        {
            glBindFramebuffer(GL_FRAMEBUFFER, mBackbufferFBOSS);
            ImGui::Render();
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        glQueryCounter(mGPUTimestampQueries[GPUTimestamps::RenderGUIEnd], GL_TIMESTAMP);

        // Blit to window's framebuffer
        glQueryCounter(mGPUTimestampQueries[GPUTimestamps::BlitToWindowStart], GL_TIMESTAMP);
        {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, mBackbufferFBOSS);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0); // default FBO
            
            bool scaled = mWindowWidth != mBackbufferWidth || mWindowHeight != mBackbufferHeight;
            glBlitFramebuffer(
                0, 0, mBackbufferWidth, mBackbufferHeight,
                0, 0, mWindowWidth, mWindowHeight,
                GL_COLOR_BUFFER_BIT, scaled ? GL_LINEAR : GL_NEAREST);

            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        glQueryCounter(mGPUTimestampQueries[GPUTimestamps::BlitToWindowEnd], GL_TIMESTAMP);

        mFirstFrame = false;
    }

    int GetRenderWidth() const override
    {
        return mBackbufferWidth;
    }

    int GetRenderHeight() const override
    {
        return mBackbufferHeight;
    }

    void* operator new(size_t sz)
    {
        void* mem = ::operator new(sz);
        memset(mem, 0, sz);
        return mem;
    }
};

IRenderer* NewRenderer()
{
    return new Renderer();
}