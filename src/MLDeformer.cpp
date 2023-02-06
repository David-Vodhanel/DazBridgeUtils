#include "MLDeformer.h"

#include <Alembic/Abc/All.h>
#include <Alembic/AbcGeom/All.h>
#include <Alembic/AbcCoreOgawa/All.h>

#include <dzobject.h>
#include <dznode.h>
#include <dzbone.h>
#include <dzfigure.h>
#include <dzvertexmesh.h>
#include <dzfacetmesh.h>
#include <dzscene.h>
#include <dzmodifier.h>
#include <dzfloatproperty.h>
#include <dzintproperty.h>


void MLDeformer::GeneratePoses(DzNode* Node, int PoseCount)
{
    dzScene->setAnimRange(DzTimeRange(0, PoseCount * dzScene->getTimeStep()));
    dzScene->setPlayRange(DzTimeRange(0, PoseCount * dzScene->getTimeStep()));

    // Get the list of bones. There will be duplicates from clothing items in the list so it's a map
    QMap<QString, QList<DzNode*>> Bones;
    GetBoneList(Node, Bones);

    // Start at frame 1.  Leave frame 0 as the reference pose.
    for (int Frame = 1; Frame < PoseCount; Frame++)
    {
        for (QMap<QString, QList<DzNode*>>::iterator BoneNameIter = Bones.begin(); BoneNameIter != Bones.end(); ++BoneNameIter)
        {
            QList<DzNode*> MatchingBones = BoneNameIter.value();

            // The first bone in the list should be from the main figure.
            QList<DzNode*>::iterator BoneIter = MatchingBones.begin();

            // Find the rotation bounds for the bone
            double nXRotationMin = (*BoneIter)->getXRotControl()->getMin();
            double nXRotationMax = (*BoneIter)->getXRotControl()->getMax();
            double nYRotationMin = (*BoneIter)->getYRotControl()->getMin();
            double nYRotationMax = (*BoneIter)->getYRotControl()->getMax();
            double nZRotationMin = (*BoneIter)->getZRotControl()->getMin();
            double nZRotationMax = (*BoneIter)->getZRotControl()->getMax();

            // Create a random rotation
            double XRotation = RandomInRange(nXRotationMin, nXRotationMax);
            double YRotation = RandomInRange(nYRotationMin, nYRotationMax);
            double ZRotation = RandomInRange(nZRotationMin, nZRotationMax);

            // Set the rotation for the frame
            (*BoneIter)->getXRotControl()->setValue(dzScene->getTimeStep() * double(Frame), XRotation);
            (*BoneIter)->getYRotControl()->setValue(dzScene->getTimeStep() * double(Frame), YRotation);
            (*BoneIter)->getZRotControl()->setValue(dzScene->getTimeStep() * double(Frame), ZRotation);
        }
    }
}

float MLDeformer::RandomInRange(float Min, float Max)
{
    float Random = rand() / double(RAND_MAX);
    Random = Random * (Max - Min);
    Random += Min;
    return Random;
}

void MLDeformer::GetBoneList(DzNode* Node, QMap<QString, QList<DzNode*>>& Bones)
{
    if (DzBone* Bone = qobject_cast<DzBone*>(Node))
    {
        // I don't think we want to do hip rotations.
        if (Node->getName() != "hip")
        {
            // Twist bones seems to have odd ranges
            if (!Node->getName().contains("twist"))
            {
                if (!Bones.contains(Node->getName()))
                {
                    Bones.insert(Node->getName(), QList<DzNode*>());
                }
                Bones[Node->getName()].append(Node);
            }
        }

        // Not doing face bones yet
        if (Node->getName() == "head") return;
    }


    // Looks through the child nodes for more bones
    for (int ChildIndex = 0; ChildIndex < Node->getNumNodeChildren(); ChildIndex++)
    {
        DzNode* ChildNode = Node->getNodeChild(ChildIndex);
        if (DzBone* ChildBone = qobject_cast<DzBone*>(ChildNode))
        {
            GetBoneList(ChildNode, Bones);
        }
    }
}

void MLDeformer::ExportTrainingData(DzNode* Node, QString FileName)
{
    // Create the Abc file and set the time to match Daz output
    Alembic::AbcCoreOgawa::WriteArchive AbcWriteArchive;
    Alembic::Abc::OArchive AbcArchive = Alembic::Abc::OArchive(AbcWriteArchive, FileName.toLatin1().constData());
    Alembic::Abc::TimeSamplingPtr TimeSampling = Alembic::Abc::TimeSamplingPtr(new Alembic::Abc::TimeSampling((double)dzScene->getTimeStep() / 4800, 0.0));

    // Get a list of the figures to export (Character, Shirt, Pants, etc)
    QList<DzNode*> FigureList;
    GetFigureList(Node, FigureList);

    // Add each figure to the archive
    foreach(DzNode * FigureNode, FigureList)
    {
        
        // Get the mesh name.  Need to add a suffix or names that end with a number won't connect in Unreal
        DzObject* Object = FigureNode->getObject();
        std::string MeshName = FigureNode->getName().toLatin1().constData();
        MeshName.append("_GeoCache");

        // Create the Alembic Mesh
        Alembic::AbcGeom::OPolyMesh MeshObj(AbcArchive.getTop(), MeshName, TimeSampling);
        Alembic::AbcGeom::OPolyMeshSchema& MeshSchema = MeshObj.getSchema();

        // Tick through the poses exporting them
        DzTimeRange PlayRange = dzScene->getPlayRange();
        for (DzTime CurrentTime = PlayRange.getStart(); CurrentTime <= PlayRange.getEnd(); CurrentTime += dzScene->getTimeStep())
        {
            // Update the frame
            DzTime Frame = CurrentTime / dzScene->getTimeStep();
            dzScene->setFrame(Frame);

            // Update the character and current figure mesh for the frame
            Node->update();
            Node->finalize();
            FigureNode->update();
            FigureNode->finalize();

            // Get the Geometry
            DzVertexMesh* DualQuaternionMesh = Object->getCachedGeom();

            // Get the vertex positions
            std::vector<Imath::V3f> AlembicVertices;
            float scaleFactor = 1.0f;
            for (int vertexID = 0; vertexID < DualQuaternionMesh->getNumVertices(); vertexID++)
            {
                AlembicVertices.push_back(Imath::V3f(DualQuaternionMesh->getVertex(vertexID)[0] * scaleFactor, DualQuaternionMesh->getVertex(vertexID)[1] * scaleFactor, DualQuaternionMesh->getVertex(vertexID)[2] * scaleFactor));
            }

            // Add the vertex positions for the frame
            Alembic::AbcGeom::OPolyMeshSchema::Sample FrameSample;
            FrameSample.setPositions(Alembic::Abc::V3fArraySample(AlembicVertices));

            // Next get the vertex indexes and count for each face
            DzFacetMesh* FacetMesh = dynamic_cast<DzFacetMesh*>(DualQuaternionMesh);

            std::vector<int> faceVertexIndices;
            std::vector<int> faceVertexCounts;
            for (int FacetIndex = 0; FacetIndex < FacetMesh->getNumFacets(); FacetIndex++)
            {
                // Add the vertex count for this face
                DzFacet Facet = FacetMesh->getFacet(FacetIndex);
                int FacetVertexCount = 3;
                if (Facet.isQuad())
                {
                    FacetVertexCount = 4;
                }
                faceVertexCounts.push_back(FacetVertexCount);

                // Add the vertex indices for this face
                for (int FacetVertexIndex = 0; FacetVertexIndex < FacetVertexCount; FacetVertexIndex++)
                {
                    faceVertexIndices.push_back(Facet.m_vertIdx[FacetVertexIndex]);
                }
            }

            // Add the face data for the frame
            FrameSample.setFaceIndices(Alembic::Abc::Int32ArraySample(faceVertexIndices));
            FrameSample.setFaceCounts(Alembic::Abc::Int32ArraySample(faceVertexCounts));

            // Add the frame to the Mesh
            MeshSchema.set(FrameSample);
        }
    }
}

void MLDeformer::GetFigureList(DzNode* Node, QList<DzNode*>& Figures)
{
    // If this is a visible figure, add it to the list
    if (Node->isVisible())
    {
        if (DzFigure* Figure = qobject_cast<DzFigure*>(Node))
        {
            // Need interactive update to let the clothing do it's "fit" pass each frame.
            TurnOnInteractiveUpdates(Node);
            Figures.append(Node);
        }
    }

    // Looks through the child nodes for more figures
    for (int ChildIndex = 0; ChildIndex < Node->getNumNodeChildren(); ChildIndex++)
    {
        DzNode* ChildNode = Node->getNodeChild(ChildIndex);
        GetFigureList(ChildNode, Figures);
    }
}

void MLDeformer::TurnOnInteractiveUpdates(DzNode* Node)
{
    // Find the Interactive Update property and turn it On
    if (Node == nullptr) return;
    if (DzObject* Object = Node->getObject())
    {
        for (int index = 0; index < Object->getNumModifiers(); index++)
        {
            DzModifier* modifier = Object->getModifier(index);
            if (modifier)
            {
                for (int propindex = 0; propindex < modifier->getNumProperties(); propindex++)
                {
                    DzProperty* property = modifier->getProperty(propindex);
                    QString propName = property->getName();
                    QString propLabel = property->getLabel();
                    if (propName == "Interactive Update")
                    {  
                        if (DzIntProperty* numericProperty = qobject_cast<DzIntProperty*>(property))
                        {
                            numericProperty->setValue(1);
                            return;
                        }
                    }
                }

            }

        }
    
    }
}