#include "VoxelTree.hpp"

#include <assert.h>
#include <math.h>

VoxelTree::VoxelTree(UniformManager* uniformManager, const Scene* scene, int resolution)
    : uniformManager_(uniformManager),
    scene_(scene),
    tileResolution_(resolution / TileSubdivisons),
    shadowMap_(scene, uniformManager, 1, resolution / TileSubdivisons),
    voxelWriter_(),
    startedTiles_(0),
    completedTiles_(0),
    activeTiles_(),
    activeTilesMutex_(),
    tilesOnGPU_(0)
{
    // Each tile must be at least 8x8 so that leaf masks can be used
    // and no more than 16K (maximum texture resolution)
    assert(tileResolution_ >= 8);
    assert(tileResolution_ <= 16384);
    
    // For now, use a dummy tree consisting of a single, fully
    // unshadowed inner node
    VoxelInnerNode node;
    node.childMask = 21845; // = 0101010101010101 = 8 Unshadowed children
    VoxelPointer nodePtr = voxelWriter_.writeNode(node, 0, 0);
    
    // Set the dummy node as the root for every tile
    for(int i = 0; i < TileSubdivisons * TileSubdivisons; ++i)
    {
        treePointers_[i] = nodePtr;
    }
    
    // Create the buffer to hold the tree
    glGenBuffers(1, &buffer_);
    glBindBuffer(GL_TEXTURE_BUFFER, buffer_);
    updateTreeBuffer();

    // Create the buffer texture
    glGenTextures(1, &bufferTexture_);
    glBindTexture(GL_TEXTURE_BUFFER, bufferTexture_);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, buffer_);
    
    // Set the initial uniform buffer values
    updateUniformBuffer();
    
    // Start the tile merging thread
    mergingThread_ = thread(&VoxelTree::mergeTiles, this);
}

VoxelTree::~VoxelTree()
{
    
}

void VoxelTree::updateBuild()
{
    // Start another tile build if the limit is not currently met
    if(activeTiles_.size() < ConcurrentBuilds)
    {
        processFirstQueuedTile();
    }
    
    // Reupload the tree to the gpu if more tiles have finished
    if(tilesOnGPU_ < completedTiles_)
    {
        tilesOnGPU_ = completedTiles_;
        
        updateUniformBuffer();
        updateTreeBuffer();
    }
    
    // Check if any tiles have finished being built
    // and are ready to be merged
    //mergeFirstFinishedTile();
}

void VoxelTree::processFirstQueuedTile()
{
    // Only continue if there are tiles left to start
    if(startedTiles_ == TileSubdivisons * TileSubdivisons)
    {
        return;
    }
    
    // Use the bounds for the next queued tile
    int tileIndex = startedTiles_;
    Bounds bounds = tileBounds(tileIndex);
    
    // Get the entry and exit depths for the tile by rendering
    // a dual shadow map.
    float* entryDepths;
    float* exitDepths;
    computeDualShadowMaps(bounds, &entryDepths, &exitDepths);
    
    // Create the builder.
    // The builder will construct the tile's tree on a background thread.
    activeTilesMutex_.lock();
    activeTiles_.push_back(new VoxelBuilder(tileIndex, tileResolution_, entryDepths, exitDepths));
    activeTilesMutex_.unlock();
    
    // Increment the started tiles count
    startedTiles_ ++;
}

void VoxelTree::mergeTiles()
{
    // Keep looking for tiles to merge until finished
    while(completedTiles_ < TileSubdivisons * TileSubdivisons)
    {
        // Look for a finished builder
        activeTilesMutex_.lock();
        VoxelBuilder* builder = findFinishedBuilder();
        activeTilesMutex_.unlock();
        
        if(builder == NULL)
        {
            continue;
        }
        
        // Gather the subtree information
        int tile = builder->tileIndex();
        uint32_t* subtree = (uint32_t*)builder->tree();
        VoxelPointer subtreeRoot = builder->rootAddress();

        // Write the tree to the combined tree and store the root node location
        VoxelPointer ptr = voxelWriter_.writeTree(subtree, subtreeRoot, tileResolution_);
        treePointers_[tile] = ptr;
        
        // The builder is no longer needed
        delete builder;
        
        // Update the completed tiles count
        completedTiles_ ++;
    }
}

VoxelBuilder* VoxelTree::findFinishedBuilder()
{
    // Look for a builder that has finished
    for(unsigned int i = 0; i < activeTiles_.size(); ++i)
    {
        // Get the builder
        VoxelBuilder* builder = activeTiles_[i];
        
        // Check the state is Done
        if(builder->buildState() == VoxelBuilderState::Done)
        {
            // Remove the builder from the active list
            std::swap(activeTiles_[i], activeTiles_.back());
            activeTiles_.pop_back();
            
            return builder;
        }
    }
    
    // No builders are finished
    return NULL;
}

void VoxelTree::updateUniformBuffer()
{
    // Cover the scene witht the shadowmap and get the world to shadow matrix
    shadowMap_.setLightSpaceBounds(sceneBoundsLightSpace());
    Matrix4x4 worldToShadow = shadowMap_.worldToShadowMatrix(0);
    
    // Scale the world to shadow matrix by the total voxel resolution
    Vector3 scale;
    scale.x = tileResolution_ * TileSubdivisons;
    scale.y = tileResolution_ * TileSubdivisons;
    scale.z = tileResolution_; // The trees are only tiled in x and y
    worldToShadow = Matrix4x4::scale(scale) * worldToShadow;
    
    // Update the uniform buffer
    VoxelsUniformBuffer buffer;
    buffer.worldToVoxels = worldToShadow;
    buffer.voxelTreeHeight = log2(tileResolution_);
    buffer.tileSubdivisions = TileSubdivisons;
    
    for(int i = 0; i < TileSubdivisons * TileSubdivisons; ++i)
    {
        buffer.rootAddresses[i*4] = treePointers_[i];
    }
    
    uniformManager_->updateVoxelBuffer(&buffer, sizeof(VoxelsUniformBuffer));
}

void VoxelTree::updateTreeBuffer()
{
    // Get the current tree data
    const void* treeData = voxelWriter_.data();
    size_t treeSizeBytes = voxelWriter_.dataSizeBytes();
    
    // Create the buffer to hold the tree
    glBufferData(GL_TEXTURE_BUFFER, treeSizeBytes, treeData, GL_STATIC_DRAW);
}

Bounds VoxelTree::sceneBoundsLightSpace() const
{
    // Get the world to light space transformation matrix (without translation)
    Matrix4x4 worldToLight = scene_->mainLight()->worldToLocal();
    worldToLight.set(0, 3, 0.0);
    worldToLight.set(1, 3, 0.0);
    worldToLight.set(2, 3, 0.0);
    
    // Create a Bounds containing the origin only.
    Bounds bounds = Bounds(Vector3::zero(), Vector3::zero());
    
    // Expand the scene bounds to cover each mesh
    const vector<MeshInstance>* instances = scene_->meshInstances();
    for(auto instance = instances->begin(); instance != instances->end(); ++instance)
    {
        // Get the model to light transformation
        Matrix4x4 modelToLight = worldToLight * instance->localToWorld();
        
        Mesh* mesh = instance->mesh();
        for(int v = 0; v < mesh->verticesCount(); ++v)
        {
            // Convert each point to light space
            Vector4 modelPos = Vector4(mesh->vertices()[v], 1.0);
            Vector4 lightPos = modelToLight * modelPos;
            
            // Ensure the bounds cover the vertex
            bounds.expandToCover(lightPos.vec3());
        }
    }
    
    // Return the final bounds
    return bounds;
}

Bounds VoxelTree::tileBounds(int index) const
{
    // Compute the bounds of the entire scene in light space
    Bounds sceneBounds = sceneBoundsLightSpace();
    
    // Compute the light space size of each tile
    float tileSizeX = sceneBounds.size().x / TileSubdivisons;
    float tileSizeY = sceneBounds.size().y / TileSubdivisons;
    
    // Get the x and y position of the tile
    int x = index / TileSubdivisons;
    int y = index % TileSubdivisons;
    
    // Determine the light space bounds of the tile
    float posX = sceneBounds.min().x + (tileSizeX * x);
    float posY = sceneBounds.min().y + (tileSizeY * y);
    Vector3 boundsMin(posX, posY, sceneBounds.min().z);
    Vector3 boundsMax(posX + tileSizeX, posY + tileSizeY, sceneBounds.max().z);

    return Bounds(boundsMin, boundsMax);
}

void VoxelTree::computeDualShadowMaps(const Bounds &bounds, float** entryDepths, float** exitDepths)
{
    // Set the shadow map to cover the correct area
    shadowMap_.setLightSpaceBounds(bounds);
    
    // Render the shadow map normally
    shadowMap_.renderCascades();
    
    // Store the depths as the shadow entry depths
    *entryDepths = new float[tileResolution_ * tileResolution_];
    glReadPixels(0, 0, tileResolution_, tileResolution_, GL_DEPTH_COMPONENT, GL_FLOAT, *entryDepths);
    
    // Render the shadow map back faces
    glCullFace(GL_FRONT);
    shadowMap_.renderCascades();
    glCullFace(GL_BACK);
    
    // Store the depths as the shadow exit depths
    *exitDepths = new float[tileResolution_ * tileResolution_];
    glReadPixels(0, 0, tileResolution_, tileResolution_, GL_DEPTH_COMPONENT, GL_FLOAT, *exitDepths);
}
