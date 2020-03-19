// Copyright 1996-2020 Cyberbotics Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "WbNodeOperations.hpp"

#include "WbBaseNode.hpp"
#include "WbDictionary.hpp"
#include "WbField.hpp"
#include "WbFileUtil.hpp"
#include "WbLog.hpp"
#include "WbMFNode.hpp"
#include "WbNode.hpp"
#include "WbNodeReader.hpp"
#include "WbNodeUtilities.hpp"
#include "WbParser.hpp"
#include "WbProject.hpp"
#include "WbRobot.hpp"
#include "WbSFNode.hpp"
#include "WbSelection.hpp"
#include "WbSolid.hpp"
#include "WbTemplateManager.hpp"
#include "WbTokenizer.hpp"
#include "WbWorld.hpp"

#include <QtCore/QFileInfo>

#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/Importer.hpp>

#include <cassert>

static bool isRegionOccupied(const WbVector3 &pos) {
  WbWorld *const world = WbWorld::instance();
  const double ls = world->worldInfo()->lineScale();
  const QList<WbSolid *> &l = world->topSolids();
  foreach (const WbSolid *const solid, l) {
    const WbVector3 &dist = solid->translation() - pos;
    if (dist.length() < ls)
      return true;
  }

  return false;
}

// simple rule to avoid that inserted or pasted robots occupy exactly the same 3d region
static void tryToAvoidIntersections(WbNode *node) {
  WbRobot *const robot = dynamic_cast<WbRobot *>(node);
  if (robot) {
    WbVector3 tr = robot->translation();
    while (isRegionOccupied(tr)) {
      const double ls = WbWorld::instance()->worldInfo()->lineScale();
      tr.setXyz(tr.x() + ls, tr.y(), tr.z() + ls);
    }
    robot->setTranslation(tr.x(), tr.y(), tr.z());
  }
}

WbNodeOperations *WbNodeOperations::cInstance = NULL;

WbNodeOperations *WbNodeOperations::instance() {
  if (!cInstance)
    cInstance = new WbNodeOperations();
  return cInstance;
}

void WbNodeOperations::cleanup() {
  delete cInstance;
  cInstance = NULL;
}

WbNodeOperations::WbNodeOperations() : mNodesAreAboutToBeInserted(false), mSkipUpdates(false), mFromSupervisor(false) {
}

void WbNodeOperations::enableSolidNameClashCheckOnNodeRegeneration(bool enabled) const {
  if (enabled)
    connect(WbTemplateManager::instance(), &WbTemplateManager::postNodeRegeneration, this,
            &WbNodeOperations::resolveSolidNameClashIfNeeded, Qt::UniqueConnection);
  else
    disconnect(WbTemplateManager::instance(), &WbTemplateManager::postNodeRegeneration, this,
               &WbNodeOperations::resolveSolidNameClashIfNeeded);
}

QString WbNodeOperations::exportNodeToString(WbNode *node) {
  QString nodeString;
  WbVrmlWriter writer(&nodeString, WbWorld::instance()->fileName());
  node->write(writer);
  return nodeString;
}

WbNodeOperations::OperationResult WbNodeOperations::importNode(int nodeId, int fieldId, int itemIndex, const QString &filename,
                                                               const QString &nodeString, int *importedNodesNumber,
                                                               bool fromSupervisor) {
  WbBaseNode *parentNode = static_cast<WbBaseNode *>(WbNode::findNode(nodeId));
  assert(parentNode);

  WbField *field = parentNode->field(fieldId);
  assert(field);

  return importNode(parentNode, field, itemIndex, filename, nodeString, false, importedNodesNumber, fromSupervisor);
}

WbNodeOperations::OperationResult WbNodeOperations::importNode(WbNode *parentNode, WbField *field, int itemIndex,
                                                               const QString &filename, const QString &nodeString,
                                                               bool avoidIntersections, int *importedNodesNumber,
                                                               bool fromSupervisor) {
  if (importedNodesNumber)
    *importedNodesNumber = 0;
  mFromSupervisor = fromSupervisor;
  WbSFNode *sfnode = dynamic_cast<WbSFNode *>(field->value());
#ifndef NDEBUG
  WbMFNode *mfnode = dynamic_cast<WbMFNode *>(field->value());
  assert(mfnode || sfnode);
  // index value is assumed to be in range [0, mfnode->size()]
  // user input checked in wb_supervisor_field_import_mf_node or WbSceneTree
  assert(!mfnode || (itemIndex >= 0 && itemIndex <= mfnode->size()));
#endif

  WbTokenizer tokenizer;
  int errors = 0;
  if (!filename.isEmpty())
    errors = tokenizer.tokenize(filename);
  else if (!nodeString.isEmpty())
    errors = tokenizer.tokenizeString(nodeString);
  else {
    mFromSupervisor = false;
    return FAILURE;
  }

  if (errors) {
    mFromSupervisor = false;
    return FAILURE;
  }

  // check syntax
  WbParser parser(&tokenizer);
  if (!parser.parseObject(WbWorld::instance()->fileName())) {
    mFromSupervisor = false;
    return FAILURE;
  }

  if (sfnode && sfnode->value() != NULL)
    // clear selection and set mSelectedItem to NULL
    WbSelection::instance()->selectTransformFromView3D(NULL);

  // read node
  WbNode::setGlobalParent(parentNode);
  WbNodeReader nodeReader;
  // set available DEF nodes to be used while reading the new nodes
  QList<WbNode *> defNodes = WbDictionary::instance()->computeDefForInsertion(parentNode, field, itemIndex, false);
  foreach (WbNode *node, defNodes)
    nodeReader.addDefNode(node);
  QList<WbNode *> nodes = nodeReader.readNodes(&tokenizer, WbWorld::instance()->fileName());
  if (sfnode && nodes.size() > 1)
    WbLog::warning(tr("Trying to import multiple nodes in the '%1' SFNode field. "
                      "Only the first node will be inserted")
                     .arg(field->name()));

  const WbNode::NodeUse nodeUse = dynamic_cast<WbBaseNode *>(parentNode)->nodeUse();
  WbBaseNode *childNode = NULL;
  bool isNodeRegenerated = false;
  int nodeIndex = itemIndex;
  foreach (WbNode *node, nodes) {
    childNode = static_cast<WbBaseNode *>(node);
    QString errorMessage;
    if (WbNodeUtilities::isAllowedToInsert(field, childNode->nodeModelName(), parentNode, errorMessage, nodeUse,
                                           WbNodeUtilities::slotType(childNode),
                                           QStringList() << childNode->nodeModelName() << childNode->modelName(), false)) {
      if (avoidIntersections)
        tryToAvoidIntersections(childNode);
      const OperationResult result = initNewNode(childNode, parentNode, field, nodeIndex, true);
      if (result == FAILURE)
        continue;
      else if (result == REGENERATION_REQUIRED)
        isNodeRegenerated = true;
      ++nodeIndex;
      if (!field->isTemplateRegenerator() && !isNodeRegenerated)
        emit nodeAdded(childNode);
      // we need to emit this signal after finalize so that the mass properties are displayed properly
      // in the scene tree.
      // FIXME: this should be removed as the emit massPropertiesChanged() should be called from within
      // the WbSolid class when actually changing the mass properties...
      // WbSolid *const solid = dynamic_cast<WbSolid*>(childNode);
      // if (solid)
      //  solid->emit massPropertiesChanged();
    } else {
      assert(!errorMessage.isEmpty());
      WbLog::error(errorMessage);
    }

    if (sfnode)
      break;
  }

  if (importedNodesNumber && sfnode && nodes.size() > 0)
    *importedNodesNumber = 1;
  else if (importedNodesNumber)
    *importedNodesNumber = nodes.size();

  mFromSupervisor = false;
  return isNodeRegenerated ? REGENERATION_REQUIRED : SUCCESS;
}

WbNodeOperations::OperationResult WbNodeOperations::importVrml(const QString &filename, int *importedNodesNumber,
                                                               bool fromSupervisor) {
  WbTokenizer tokenizer;
  int errors = tokenizer.tokenize(filename);
  if (errors)
    return FAILURE;

  QFileInfo vrmlFile(filename);
  // check that the file we're importing VRML to is not "unnamed.wbt"
  if (WbWorld::instance()->isUnnamed())
    WbLog::error(QString("Textures could not be imported as this world has not been saved for the first time. Please save and "
                         "reload the world, then try importing again."));
  else
    // copy textures folder (if any)
    WbFileUtil::copyDir(vrmlFile.absolutePath() + "/textures", WbProject::current()->worldsPath() + "/textures", true, true,
                        true);

  // check syntax
  WbParser parser(&tokenizer);
  if (!parser.parseVrml(WbWorld::instance()->fileName()))
    return FAILURE;

  // if even one node is successfully imported, this function should return
  // true, as this implies consequently that the world was modified
  OperationResult result = FAILURE;

  // read node
  QString errorMessage;
  WbGroup *root = WbWorld::instance()->root();
  WbNode::setGlobalParent(root);
  WbNodeReader nodeReader;
  QList<WbNode *> nodes = nodeReader.readVrml(&tokenizer, WbWorld::instance()->fileName());
  WbBaseNode *lastBaseNodeCreated = NULL;
  int numberOfNodes = 0;
  foreach (WbNode *node, nodes) {
    WbBaseNode *baseNode = static_cast<WbBaseNode *>(node);
    if (WbNodeUtilities::isSingletonTypeName(baseNode->nodeModelName())) {
      WbLog::warning(QString("Skipped %1 node (to avoid duplicate) while importing VRML97.").arg(baseNode->nodeModelName()));
      delete baseNode;
    } else {
      if (WbNodeUtilities::isAllowedToInsert(root->findField("children"), baseNode->nodeModelName(), root, errorMessage,
                                             WbNode::STRUCTURE_USE, WbNodeUtilities::slotType(baseNode),
                                             QStringList(baseNode->nodeModelName()))) {
        baseNode->validate();
        root->addChild(baseNode);
        baseNode->finalize();
        lastBaseNodeCreated = baseNode;
        result = SUCCESS;
        numberOfNodes++;
      } else {
        WbLog::error(errorMessage);
        delete baseNode;
      }
    }
  }
  if (importedNodesNumber)
    *importedNodesNumber = numberOfNodes;
  if (lastBaseNodeCreated && !fromSupervisor)
    WbSelection::instance()->selectNodeFromSceneTree(lastBaseNodeCreated);
  return result;
}

void addModelNode(QString &stream, const aiNode *node, const aiScene *scene, const QString &referenceFolder) {
  aiVector3t<float> scaling, position;
  aiQuaternion rotation;
  node->mTransformation.Decompose(scaling, rotation, position);

  stream += " Solid { ";
  stream += QString(" translation %1 %2 %3 ").arg(position[0]).arg(position[1]).arg(position[2]);
  // TODO: rotation
  stream += QString(" scale %1 %2 %3 ").arg(scaling[0]).arg(scaling[1]).arg(scaling[2]);
  stream += QString(" children [");

  const bool defNeedGroup = node->mNumMeshes > 1;

  if (defNeedGroup) {
    stream += " DEF SHAPE Group { ";
    stream += " children [ ";
  }

  for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
    const aiMesh *mesh = scene->mMeshes[node->mMeshes[i]];
    const aiMaterial *material = scene->mMaterials[mesh->mMaterialIndex];
    if (defNeedGroup)
      stream += " Shape { ";
    else
      stream += " DEF SHAPE Shape { ";
    // extract the appearance
    stream += " appearance PBRAppearance { ";
    WbVector3 baseColor(1.0, 1.0, 1.0), emissiveColor(0.0, 0.0, 0.0);
    QString name("PBRAppearance");
    float roughness = 1.0, transparency = 0.0;
    for (unsigned int j = 0; j < material->mNumProperties; ++j) {
      const aiMaterialProperty *property = material->mProperties[j];
      const QString propertyName(property->mKey.C_Str());
      if (propertyName.endsWith("diffuse") && property->mType == aiPTI_Float) {
        assert(property->mDataLength == 3 * sizeof(float));
        const float *diffuse = (float *)property->mData;
        baseColor = WbVector3(diffuse[0], diffuse[1], diffuse[2]);
      }
      if (propertyName.endsWith("emissive") && property->mType == aiPTI_Float) {
        assert(property->mDataLength == 3 * sizeof(float));
        const float *emissive = (float *)property->mData;
        emissiveColor = WbVector3(emissive[0], emissive[1], emissive[2]);
      }
      if (propertyName.endsWith("shinpercent") && property->mType == aiPTI_Float) {
        assert(property->mDataLength == sizeof(float));
        roughness = 0.01 * (100.0 - *((float *)property->mData));
      }
      if (propertyName.endsWith("opacity")  && property->mType == aiPTI_Float) {
        assert(property->mDataLength == sizeof(float));
        transparency = 1.0 - *((float *)property->mData);
      }
      if (propertyName.endsWith("name") && property->mType == aiPTI_String) {
        aiString nameProperty;
        if (aiGetMaterialString(material, property->mKey.C_Str(), property->mType, property->mIndex, &nameProperty) == aiReturn_SUCCESS)
          name = nameProperty.C_Str();
      }
      // Uncomment this part to print all the properties of this material
      // qDebug() << propertyName << property->mData
      //          << property->mSemantic << property->mIndex
      //          << property->mDataLength << property->mType;
    }
    stream += " baseColor " + baseColor.toString(WbPrecision::FLOAT_MAX);
    stream += " emissiveColor " + emissiveColor.toString(WbPrecision::FLOAT_MAX);
    stream += " name \"" + name + "\"";
    stream += " metalness 0";
    stream += QString(" transparency %1").arg(transparency);
    stream += QString(" roughness %1").arg(roughness);
    if (material->GetTextureCount(aiTextureType_DIFFUSE) > 0) {
      aiString path;
      material->GetTexture(aiTextureType_DIFFUSE, 0, &path);
      QString texturePath(path.C_Str());
      texturePath.replace("\\", "\\\\");
      if (!QFile::exists(texturePath) && QFile::exists(referenceFolder + texturePath))
        texturePath = referenceFolder + texturePath;  // if absolute path doesn't exist, try with relative
      stream += " baseColorMap ImageTexture { ";
      stream += " url [ ";
      stream += " \"" + texturePath + "\" ";
      stream += " ] ";
      stream += " } ";
    }
    if (material->GetTextureCount(aiTextureType_NORMALS) > 0) {
      aiString path;
      material->GetTexture(aiTextureType_NORMALS, 0, &path);
      QString texturePath(path.C_Str());
      texturePath.replace("\\", "\\\\");
      if (!QFile::exists(texturePath) && QFile::exists(referenceFolder + texturePath))
        texturePath = referenceFolder + texturePath;  // if absolute path doesn't exist, try with relative
      stream += " normalMap ImageTexture { ";
      stream += " url [ ";
      stream += " \"" + texturePath + "\" ";
      stream += " ] ";
      stream += " } ";
    }
    stream += " } ";
    // extract the geometry
    stream += " geometry IndexedFaceSet { ";
    stream += " coord Coordinate { ";
    stream += " point [ ";
    for (unsigned int j = 0; j < mesh->mNumVertices; ++j) {
      const aiVector3D vertice = mesh->mVertices[j];
      stream += QString(" %1 %2 %3,").arg(vertice[0]).arg(vertice[1]).arg(vertice[2]);
    }
    stream += " ]";
    stream += " } ";
    if (mesh->HasNormals()) {
      stream += " normal Normal { ";
      stream += " vector [ ";
      for (unsigned int j = 0; j < mesh->mNumVertices; ++j) {
        const aiVector3D normal = mesh->mNormals[j];
        stream += QString(" %1 %2 %3,").arg(normal[0]).arg(normal[1]).arg(normal[2]);
      }
      stream += " ]";
      stream += " } ";
    }
    if (mesh->HasTextureCoords(0)) {
      stream += " texCoord TextureCoordinate { ";
      stream += " point [ ";
      for (unsigned int j = 0; j < mesh->mNumVertices; ++j) {
        const aiVector3D texCoord = mesh->mTextureCoords[0][j];
        stream += QString(" %1 %2,").arg(texCoord[0]).arg(texCoord[1]);
      }
      stream += " ]";
      stream += " } ";
    }
    stream += " coordIndex [ ";
    for (unsigned int j = 0; j < mesh->mNumFaces; ++j) {
      const aiFace face = mesh->mFaces[j];
      stream += QString(" %1 %2 %3 -1").arg(face.mIndices[0]).arg(face.mIndices[1]).arg(face.mIndices[2]);
    }
    stream += " ]";
    stream += " } ";
    stream += " } ";
  }

  if (defNeedGroup) {
    stream += " ] ";
    stream += " } ";
  }

  for (unsigned int i = 0; i < node->mNumChildren; ++i)
    addModelNode(stream, node->mChildren[i], scene, referenceFolder);

  stream += " ] ";
  stream += QString(" name \"%1\" ").arg(node->mName.C_Str());
  if (node->mNumMeshes > 0)
    stream += " boundingObject USE SHAPE ";
  stream += " } ";
}

WbNodeOperations::OperationResult WbNodeOperations::importExternalModel(const QString &filename) {
  OperationResult result = FAILURE;
  Assimp::Importer importer;
  importer.SetPropertyInteger(AI_CONFIG_PP_RVC_FLAGS, aiComponent_CAMERAS | aiComponent_LIGHTS | aiComponent_BONEWEIGHTS |
                                                      aiComponent_ANIMATIONS);
  const aiScene *scene = importer.ReadFile(
    filename.toStdString().c_str(), aiProcess_ValidateDataStructure | aiProcess_Triangulate |
                                    aiProcess_JoinIdenticalVertices | aiProcess_RemoveComponent);
  if (!scene) {
    WbLog::warning(tr("Invalid data, please verify mesh file (bone weights, normals, ...): %1").arg(importer.GetErrorString()));
    return result;
  }

  QString stream = "";
  for (unsigned int i = 0; i < scene->mRootNode->mNumChildren; ++i)
    addModelNode(stream, scene->mRootNode->mChildren[i], scene, QFileInfo(filename).dir().absolutePath());
  WbGroup *root = WbWorld::instance()->root();
  result = importNode(root, root->findField("children"), root->childCount(), QString(), stream);

  return result;
}

WbNodeOperations::OperationResult WbNodeOperations::initNewNode(WbNode *newNode, WbNode *parentNode, WbField *field,
                                                                int newNodeIndex, bool subscribe) {
  const bool isInBoundingObject = dynamic_cast<WbSolid *>(parentNode) && field->name() == "boundingObject";
  if (!WbNodeUtilities::validateInsertedNode(field, newNode, parentNode, isInBoundingObject)) {
    delete newNode;
    return FAILURE;
  }

  WbBaseNode *const baseNode = dynamic_cast<WbBaseNode *>(newNode);
  // set parent node
  newNode->setParent(parentNode);
  WbNode *upperTemplate = WbNodeUtilities::findUpperTemplateNeedingRegenerationFromField(field, parentNode);
  bool isInsideATemplateRegenerator = upperTemplate && (upperTemplate != baseNode);

  // insert in parent field
  mNodesAreAboutToBeInserted = true;
  WbMFNode *const mfnode = dynamic_cast<WbMFNode *>(field->value());
  if (mfnode) {
    if (isInsideATemplateRegenerator) {
      mfnode->blockSignals(true);  // otherwise, the node regeneration is called too early
      mfnode->insertItem(newNodeIndex, newNode);
      upperTemplate->regenerateNode();
    } else
      mfnode->insertItem(newNodeIndex, newNode);

  } else {
    WbSFNode *const sfnode = dynamic_cast<WbSFNode *>(field->value());
    if (sfnode)
      sfnode->setValue(newNode);
  }
  mNodesAreAboutToBeInserted = false;

  // in case of template the newNode/baseNode pointers are no more available here
  // because the template node was regenerated, the node was finalized,
  // and the scene tree was updated
  if (isInsideATemplateRegenerator)
    return REGENERATION_REQUIRED;

  // update flag for PROTO nodes and their instances if any
  baseNode->updateNestedProtoFlag();
  baseNode->finalize();

  assert(!WbWorld::instance()->isLoading());
  if (!WbWorld::instance()->isLoading())
    resolveSolidNameClashIfNeeded(newNode);

  if (subscribe && baseNode->isTemplate())
    WbTemplateManager::instance()->subscribe(newNode);

  updateDictionary(baseNode->isUseNode(), baseNode);

  return SUCCESS;
}

void WbNodeOperations::resolveSolidNameClashIfNeeded(WbNode *node) const {
  QList<WbSolid *> solidNodes;
  WbSolid *solidNode = dynamic_cast<WbSolid *>(node);
  if (solidNode)
    solidNodes << solidNode;
  else
    solidNodes << WbNodeUtilities::findSolidDescendants(node);
  while (!solidNodes.isEmpty()) {
    WbSolid *s = solidNodes.takeFirst();
    const WbBaseNode *const parentBaseNode = dynamic_cast<WbBaseNode *>(s->parent());
    const WbSolid *parentSolidNode = dynamic_cast<const WbSolid *>(parentBaseNode);
    const WbSolid *upperSolid = parentSolidNode ? parentSolidNode : parentBaseNode->upperSolid();
    s->resolveNameClashIfNeeded(true, true,
                                upperSolid ? upperSolid->solidChildren().toList() : WbWorld::instance()->topSolids(), NULL);
  }
}

bool WbNodeOperations::deleteNode(WbNode *node, bool fromSupervisor) {
  if (node == NULL)
    return false;

  mFromSupervisor = fromSupervisor;

  if (dynamic_cast<WbSolid *>(node))
    WbWorld::instance()->awake();

  bool dictionaryNeedsUpdate = node->hasAreferredDefNodeDescendant();
  WbField *parentField = node->parentField();
  assert(parentField);
  WbSFNode *sfnode = dynamic_cast<WbSFNode *>(parentField->value());
  WbMFNode *mfnode = dynamic_cast<WbMFNode *>(parentField->value());
  assert(sfnode || mfnode);
  WbNodeOperations::instance()->notifyNodeDeleted(node);
  bool success;
  if (sfnode) {
    sfnode->setValue(NULL);
    success = true;
  } else {
    assert(mfnode);
    success = mfnode->removeNode(node);
    delete node;
  }

  if (success && dictionaryNeedsUpdate)
    updateDictionary(false, NULL);

  mFromSupervisor = false;
  return success;
}

void WbNodeOperations::requestUpdateDictionary() {
  updateDictionary(false, NULL);
}

void WbNodeOperations::updateDictionary(bool load, WbBaseNode *protoRoot) {
  mSkipUpdates = true;
  WbNode::setDictionaryUpdateFlag(true);
  WbDictionary *dictionary = WbDictionary::instance();
  dictionary->update(load);  // update all DEF-USE dependencies
  if (protoRoot && !protoRoot->isUseNode())
    dictionary->updateProtosPrivateDef(protoRoot);
  WbNode::setDictionaryUpdateFlag(false);
  mSkipUpdates = false;
}

void WbNodeOperations::requestUpdateSceneDictionary(WbNode *node, bool fromUseToDef) {
  WbDictionary::instance()->updateNodeDefName(node, fromUseToDef);
}

void WbNodeOperations::notifyNodeAdded(WbNode *node) {
  emit nodeAdded(node);
}

void WbNodeOperations::notifyNodeDeleted(WbNode *node) {
  emit nodeDeleted(node);
}
