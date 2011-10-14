/*==============================================================================

  Program: 3D Slicer

  Copyright (c) Kitware Inc.

  See COPYRIGHT.txt
  or http://www.slicer.org/copyright/copyright.txt for details.

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  This file was originally developed by Julien Finet, Kitware Inc.
  and was partially funded by NIH grant 3P41RR013218-12S1

==============================================================================*/

// MRMLDisplayableManager includes
#include "vtkMRMLCrosshairDisplayableManager.h"

// MRML includes
#include <vtkMRMLColorNode.h>
#include <vtkMRMLCrosshairNode.h>
#include <vtkMRMLDisplayNode.h>
#include <vtkMRMLInteractionNode.h>
#include <vtkMRMLLightBoxRendererManagerProxy.h>
#include <vtkMRMLSliceCompositeNode.h>
#include <vtkMRMLSliceNode.h>

// VTK includes
#include <vtkActor2D.h>
#include <vtkCallbackCommand.h>
#include <vtkCellArray.h>
#include <vtkMatrix4x4.h>
#include <vtkNew.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper2D.h>
#include <vtkProp.h>
#include <vtkProperty2D.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSmartPointer.h>
#include <vtkWeakPointer.h>

// STD includes
#include <algorithm>
#include <cassert>

//---------------------------------------------------------------------------
vtkStandardNewMacro(vtkMRMLCrosshairDisplayableManager );
vtkCxxRevisionMacro(vtkMRMLCrosshairDisplayableManager, "$Revision: 13525 $");

//---------------------------------------------------------------------------
class vtkMRMLCrosshairDisplayableManager::vtkInternal 
{
public:
  vtkInternal(vtkMRMLCrosshairDisplayableManager * external);
  ~vtkInternal();

  // Slice
  vtkMRMLSliceNode* GetSliceNode();
  void UpdateSliceNode();
  // Slice Composite
  vtkMRMLSliceCompositeNode* FindSliceCompositeNode();
  void SetSliceCompositeNode(vtkMRMLSliceCompositeNode* compositeNode);
  // Crosshair
  vtkMRMLCrosshairNode* FindCrosshairNode();
  void SetCrosshairNode(vtkMRMLCrosshairNode* crosshairNode);

  // Actors
  void SetActor(vtkActor2D* prop) {Actor = prop;};
  
  // Build the crosshair representation
  void BuildCrosshair();

  // Add a line to the crosshair in display coordinates (needs to be
  // passed the points and cellArray to manipulate).
  void AddCrosshairLine(vtkPoints *pts, vtkCellArray *cellArray, 
                        int p1x, int p1y, int p2x, int p2y);

  // Did crosshair position change?
  bool DidCrosshairPositionChange();

  // Did crosshair property change?
  bool DidCrosshairPropertyChange();

  vtkMRMLCrosshairDisplayableManager*      External;  
  vtkWeakPointer<vtkMRMLSliceCompositeNode> SliceCompositeNode;
  vtkWeakPointer<vtkMRMLCrosshairNode>    CrosshairNode;
  vtkSmartPointer<vtkActor2D>                Actor;
  vtkSmartPointer<vtkActor2D>                HighlightActor;
  vtkSmartPointer<vtkMRMLCrosshairNode>      CrosshairNodeCache;
  vtkWeakPointer<vtkRenderer>                LightBoxRenderer;
  vtkWeakPointer<vtkMRMLLightBoxRendererManagerProxy> LightBoxRendererManagerProxy;
};

//---------------------------------------------------------------------------
// vtkInternal methods

//---------------------------------------------------------------------------
vtkMRMLCrosshairDisplayableManager::vtkInternal
::vtkInternal(vtkMRMLCrosshairDisplayableManager * external)
{
  this->External = external;
  this->SliceCompositeNode = 0;
  this->CrosshairNode = 0;
  this->Actor = 0;
  this->HighlightActor = 0;
  this->LightBoxRenderer = 0;
  this->LightBoxRendererManagerProxy = 0;
  this->CrosshairNodeCache = vtkMRMLCrosshairNode::New();
}

//---------------------------------------------------------------------------
vtkMRMLCrosshairDisplayableManager::vtkInternal::~vtkInternal()
{
  this->SetSliceCompositeNode(0);
  this->SetCrosshairNode(0);
  this->LightBoxRenderer = 0;
  this->LightBoxRendererManagerProxy = 0;
  // everything should be empty
  assert(this->SliceCompositeNode == 0);
}


//---------------------------------------------------------------------------
bool vtkMRMLCrosshairDisplayableManager::vtkInternal::DidCrosshairPositionChange()
{
  if (this->CrosshairNode.GetPointer() == 0)
    {
    return false;
    }

  double *cacheRAS = this->CrosshairNodeCache->GetCrosshairRAS();
  double *ras = this->CrosshairNode->GetCrosshairRAS();
  double eps = 1.0e-12;

  if (fabs(cacheRAS[0] - ras[0]) < eps
      && fabs(cacheRAS[1] - ras[1]) < eps
      && fabs(cacheRAS[2] - ras[2]) < eps)
    {
    return false;
    }
  else
    {
    return true;
    }
}

//---------------------------------------------------------------------------
bool vtkMRMLCrosshairDisplayableManager::vtkInternal::DidCrosshairPropertyChange()
{
  if (this->CrosshairNode.GetPointer() == 0)
    {
    return false;
    }

  bool change = false;

  if (this->CrosshairNodeCache->GetCrosshairMode() != this->CrosshairNode->GetCrosshairMode())
    {
    change = true;
    }

  if (this->CrosshairNodeCache->GetCrosshairThickness() != this->CrosshairNode->GetCrosshairThickness())
    {
    change = true;
    }

  return change;
}


//---------------------------------------------------------------------------
vtkMRMLSliceNode* vtkMRMLCrosshairDisplayableManager::vtkInternal
::GetSliceNode()
{
  return this->External->GetMRMLSliceNode();
}

//---------------------------------------------------------------------------
void vtkMRMLCrosshairDisplayableManager::vtkInternal::UpdateSliceNode()
{
  assert(!this->GetSliceNode() || this->GetSliceNode()->GetLayoutName());
  // search the scene for a matching slice composite node
  if (!this->SliceCompositeNode.GetPointer() || // the slice composite has been deleted
      !this->SliceCompositeNode->GetLayoutName() || // the slice composite points to a diff slice node
      strcmp(this->SliceCompositeNode->GetLayoutName(),
             this->GetSliceNode()->GetLayoutName()))
    {
    vtkMRMLSliceCompositeNode* sliceCompositeNode =
      this->FindSliceCompositeNode();
    this->SetSliceCompositeNode(sliceCompositeNode);
    }

  // search for the Crosshair node
  vtkMRMLCrosshairNode* crosshairNode = this->FindCrosshairNode();
  this->SetCrosshairNode(crosshairNode);
}

//---------------------------------------------------------------------------
vtkMRMLSliceCompositeNode* vtkMRMLCrosshairDisplayableManager::vtkInternal
::FindSliceCompositeNode()
{
  if (this->External->GetMRMLScene() == 0 ||
      this->GetSliceNode() == 0)
    {
    return 0;
    }

  vtkMRMLNode* node;
  vtkCollectionSimpleIterator it;
  vtkCollection* scene = this->External->GetMRMLScene()->GetCurrentScene();
  for (scene->InitTraversal(it);
       (node = (vtkMRMLNode*)scene->GetNextItemAsObject(it)) ;)
    {
    vtkMRMLSliceCompositeNode* sliceCompositeNode =
      vtkMRMLSliceCompositeNode::SafeDownCast(node);
    if (sliceCompositeNode && sliceCompositeNode->GetLayoutName() &&
        !strcmp(sliceCompositeNode->GetLayoutName(),
                this->GetSliceNode()->GetLayoutName()) )
      {
      return sliceCompositeNode;
      }
    }
  // no matching slice composite node is found
  assert(0);
  return 0;
}

//---------------------------------------------------------------------------
void vtkMRMLCrosshairDisplayableManager::vtkInternal
::SetSliceCompositeNode(vtkMRMLSliceCompositeNode* compositeNode)
{
  if (this->SliceCompositeNode == compositeNode)
    {
    return;
    }
  if (this->SliceCompositeNode)
    {
    this->SliceCompositeNode->RemoveObserver(
      this->External->GetMRMLCallbackCommand());
    }
  this->SliceCompositeNode = compositeNode;
  if (this->SliceCompositeNode)
    {
    this->SliceCompositeNode->AddObserver(vtkCommand::ModifiedEvent,
                                          this->External->GetMRMLCallbackCommand());
    }
}

//---------------------------------------------------------------------------
void vtkMRMLCrosshairDisplayableManager::vtkInternal
::SetCrosshairNode(vtkMRMLCrosshairNode* crosshairNode)
{
  if (this->CrosshairNode == crosshairNode)
    {
    return;
    }
  if (this->CrosshairNode)
    {
    this->CrosshairNode->RemoveObserver(
      this->External->GetMRMLCallbackCommand());
    }
  this->CrosshairNode = crosshairNode;
  if (this->CrosshairNode)
    {
    this->CrosshairNode->AddObserver(vtkCommand::ModifiedEvent,
                                     this->External->GetMRMLCallbackCommand());
    }
}

//---------------------------------------------------------------------------
vtkMRMLCrosshairNode* vtkMRMLCrosshairDisplayableManager::vtkInternal
::FindCrosshairNode()
{
  if (this->External->GetMRMLScene() == 0)
    {
    return 0;
    }

  vtkMRMLNode* node;
  vtkCollectionSimpleIterator it;
  vtkCollection* crosshairs = this->External->GetMRMLScene()->GetNodesByClass("vtkMRMLCrosshairNode");
  for (crosshairs->InitTraversal(it);
       (node = (vtkMRMLNode*)crosshairs->GetNextItemAsObject(it)) ;)
    {
    vtkMRMLCrosshairNode* crosshairNode = 
      vtkMRMLCrosshairNode::SafeDownCast(node);
    if (crosshairNode 
        && crosshairNode->GetCrosshairName() == std::string("default"))
      {
      return crosshairNode;
      }
    }
  // no matching crosshair node is found
  assert(0);
  return 0;
}

//---------------------------------------------------------------------------
void vtkMRMLCrosshairDisplayableManager::vtkInternal::BuildCrosshair()
{
  if (!this->CrosshairNode.GetPointer())
    {
    return;
    }

  // Remove the old actor is any
  if (this->Actor.GetPointer())
    {
    if (this->LightBoxRenderer)
      {
      this->LightBoxRenderer->RemoveActor(this->Actor);
      }
    this->Actor = 0;
    }
  
  // Get the size of the window
  int *screenSize = this->External->GetInteractor()->GetRenderWindow()->GetScreenSize();

  // Constants in display coordinates to define the crosshair
  int negW = -1.0*screenSize[0];
  int negWminus = -5;
  int negWminus2 = -10;
  int posWplus = 5;
  int posWplus2 = 10;
  int posW = screenSize[0];

  int negH = -1.0*screenSize[1];
  int negHminus = -5;
  int negHminus2 = -10;
  int posHplus = 5;
  int posHplus2 = 10;
  int posH = screenSize[1];

  // Set up the VTK data structures
  vtkNew<vtkPolyData> polyData;
  vtkNew<vtkCellArray> cellArray;
  vtkNew<vtkPoints> points;
  polyData->SetLines(cellArray.GetPointer());
  polyData->SetPoints(points.GetPointer());

  vtkNew<vtkPolyDataMapper2D> mapper;
  vtkNew<vtkActor2D> actor;
  mapper->SetInput(polyData.GetPointer());
  actor->SetMapper(mapper.GetPointer());

  if (this->LightBoxRenderer)
    {
    this->LightBoxRenderer->AddActor(actor.GetPointer());
    }
  
  // Cache the actor 
  this->SetActor(actor.GetPointer());
  
  // Define the geometry
  switch (this->CrosshairNode->GetCrosshairMode())
    {
    case vtkMRMLCrosshairNode::NoCrosshair: 
      break;
    case vtkMRMLCrosshairNode::ShowBasic: 
      this->AddCrosshairLine(points.GetPointer(), cellArray.GetPointer(), 
                             0, negH, 0, negHminus);
      this->AddCrosshairLine(points.GetPointer(), cellArray.GetPointer(), 
                             0, posHplus, 0, posH);
      this->AddCrosshairLine(points.GetPointer(), cellArray.GetPointer(), 
                             negW, 0, negWminus, 0);
      this->AddCrosshairLine(points.GetPointer(), cellArray.GetPointer(),
                             posWplus, 0, posW, 0);
      break;
    case vtkMRMLCrosshairNode::ShowIntersection: 
      this->AddCrosshairLine(points.GetPointer(), cellArray.GetPointer(), 
                             negW, 0, posW, 0);
      this->AddCrosshairLine(points.GetPointer(), cellArray.GetPointer(), 
                             0, negH, 0, posH);
      break;
    case vtkMRMLCrosshairNode::ShowSmallBasic: 
      this->AddCrosshairLine(points.GetPointer(), cellArray.GetPointer(), 
                             0, negHminus2, 0, negHminus);
      this->AddCrosshairLine(points.GetPointer(), cellArray.GetPointer(), 
                             0, posHplus, 0, posHplus2);
      this->AddCrosshairLine(points.GetPointer(), cellArray.GetPointer(), 
                             negWminus2, 0, negWminus, 0);
      this->AddCrosshairLine(points.GetPointer(), cellArray.GetPointer(), 
                             posWplus, 0, posWplus2, 0);
      break;
    case vtkMRMLCrosshairNode::ShowSmallIntersection: 
      this->AddCrosshairLine(points.GetPointer(), cellArray.GetPointer(), 
                             0, negHminus2, 0, posHplus2);
      this->AddCrosshairLine(points.GetPointer(), cellArray.GetPointer(), 
                             negWminus2, 0, posWplus2, 0);
      break;
    default: 
      break;
    }

  // Set the properties
  //
  
  // Line Width
  if (this->CrosshairNode->GetCrosshairThickness() == vtkMRMLCrosshairNode::Fine)
    {
    actor->GetProperty()->SetLineWidth(1);
    }
  else if (this->CrosshairNode->GetCrosshairThickness() == vtkMRMLCrosshairNode::Medium)
    {
    actor->GetProperty()->SetLineWidth(3);
    }
  else if (this->CrosshairNode->GetCrosshairThickness() == vtkMRMLCrosshairNode::Thick)
    {
    actor->GetProperty()->SetLineWidth(5);
    }

  // Color
  actor->GetProperty()->SetColor(1.0, 0.8, 0.1);
  actor->GetProperty()->SetOpacity(1.0);


  // Set the visibility
  if (this->CrosshairNode->GetCrosshairMode() == vtkMRMLCrosshairNode::NoCrosshair)
    {
    actor->VisibilityOff();
    }
  else
    {
    actor->VisibilityOn();
    }
}

//---------------------------------------------------------------------------
void vtkMRMLCrosshairDisplayableManager::vtkInternal::AddCrosshairLine(vtkPoints *pts, vtkCellArray *cellArray, int p1x, int p1y, int p2x, int p2y)
{
  vtkIdType p1 = pts->InsertNextPoint(p1x, p1y, 0);
  vtkIdType p2 = pts->InsertNextPoint(p2x, p2y, 0);

  cellArray->InsertNextCell(2);
  cellArray->InsertCellPoint(p1);
  cellArray->InsertCellPoint(p2);
}


//---------------------------------------------------------------------------
// vtkMRMLCrosshairDisplayableManager methods

//---------------------------------------------------------------------------
vtkMRMLCrosshairDisplayableManager::vtkMRMLCrosshairDisplayableManager()
{
  this->Internal = new vtkInternal(this);
}

//---------------------------------------------------------------------------
vtkMRMLCrosshairDisplayableManager::~vtkMRMLCrosshairDisplayableManager()
{
  delete this->Internal;
}

//---------------------------------------------------------------------------
void vtkMRMLCrosshairDisplayableManager::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
}

//---------------------------------------------------------------------------
void vtkMRMLCrosshairDisplayableManager::ProcessMRMLEvents(vtkObject * caller,
                                                            unsigned long event,
                                                            void *callData)
{
  if (caller == this->GetMRMLScene()
      && this->GetMRMLScene()->GetIsUpdating())
    {
    return;
    }
  if (event == vtkMRMLScene::SceneImportedEvent ||
      event == vtkMRMLScene::SceneRestoredEvent)
    {
    this->Internal->UpdateSliceNode();
    return;
    }
  if (event == vtkCommand::ModifiedEvent)
    {
    if (vtkMRMLCrosshairNode::SafeDownCast(caller))
      {
      //std::cout << "Crosshair node modified: " << caller << ", " << this->Internal->CrosshairNode << std::endl;

      // update the properties and style of the crosshair 
      bool builtCrosshair = false;
      if (this->Internal->DidCrosshairPropertyChange())
        {
        this->Internal->BuildCrosshair();
        builtCrosshair = true;
        }
      
      // update the position of the actor
      if ((this->Internal->DidCrosshairPositionChange() || builtCrosshair) 
          && this->Internal->Actor)
        {
        vtkMatrix4x4 *rasToXYZ = vtkMatrix4x4::New();
        rasToXYZ->DeepCopy(this->Internal->GetSliceNode()->GetXYToRAS());
        rasToXYZ->Invert();

        double *ras, rasw[4], pos[4];
        ras = this->Internal->CrosshairNode->GetCrosshairRAS();
        rasw[0] = ras[0]; rasw[1] = ras[1]; rasw[2] = ras[2]; rasw[3] = 1.0;
        rasToXYZ->MultiplyPoint(rasw, pos);
        pos[0] /= pos[3]; pos[1] /= pos[3]; pos[2] /= pos[3]; pos[3] /= pos[3];
        this->Internal->Actor->SetPosition(pos[0], pos[1]);

        rasToXYZ->Delete();

        // put the actor in the right lightbox
        if (this->Internal->LightBoxRendererManagerProxy)
          {
          vtkRenderer *renderer 
            = this->Internal->LightBoxRendererManagerProxy->GetRenderer(pos[2]);
          if (renderer != this->Internal->LightBoxRenderer)
            {
            if (this->Internal->LightBoxRenderer)
              {
              this->Internal->LightBoxRenderer
                ->RemoveActor(this->Internal->Actor);
              }
            if (renderer)
              {
              renderer->AddActor(this->Internal->Actor);
              }
            this->Internal->LightBoxRenderer = renderer;
            }
          }        
        
        //std::cout << this->Internal->GetSliceNode()->GetSingletonTag() << " -- RAS: " << this->Internal->CrosshairNode->GetCrosshairRAS()[0] << ", " << this->Internal->CrosshairNode->GetCrosshairRAS()[1] << ", " << this->Internal->CrosshairNode->GetCrosshairRAS()[2] << ", XYZ = " << pos[0] << ", " << pos[1] << ", " << pos[2] << std::endl;
        }

      // Update the cache of the crosshair
      this->Internal->CrosshairNodeCache->Copy(this->Internal->CrosshairNode);
      }

    // if (vtkMRMLSliceCompositeNode::SafeDownCast(caller))
    //   {
    // //std::cout << "SLICE COMPOSITE UPDATED" << std::endl;
    //   this->Internal->UpdateSliceCompositeNode(vtkMRMLSliceCompositeNode::SafeDownCast(caller));
    //   }
    // else if (vtkMRMLDisplayableNode::SafeDownCast(caller))
    //   {
    //   //std::cout << "VOLUME UPDATED" << std::endl;
    //   if (callData == 0)
    //     {
    //     this->Internal->UpdateVolume(vtkMRMLDisplayableNode::SafeDownCast(caller));
    //     }
    //   }
    // else if (vtkMRMLDisplayNode::SafeDownCast(caller))
    //   {
    //   //std::cout << "DISPLAY NODE UPDATED" << std::endl;
    //   this->Internal->UpdateVolumeDisplayNode(vtkMRMLDisplayNode::SafeDownCast(caller));
    //   }
    }
  // Default MRML Event handler is NOT needed
//  else
//    {
//    this->Superclass::ProcessMRMLEvents(caller, event, callData);
//    }

  // Request a render 
  this->RequestRender();
}

//---------------------------------------------------------------------------
void vtkMRMLCrosshairDisplayableManager::Create()
{
  // Setup the SliceNode, SliceCompositeNode, CrosshairNode
  this->Internal->UpdateSliceNode();
}

//---------------------------------------------------------------------------
void vtkMRMLCrosshairDisplayableManager::OnInteractorStyleEvent(int eventid)
{
   // std::cout << "InteractorStyle event: " << eventid
   //           << ", eventname:" << vtkCommand::GetStringFromEventId(eventid) 
   //           << std::endl;

  this->Superclass::OnInteractorStyleEvent(eventid);

  // Compute which lightbox and determine the RAS position of the
  // crosshair, then set the RAS position on the Crosshair node
  if (this->Internal->CrosshairNode)
    {
    int *pos =  this->GetInteractor()->GetEventPosition();

    // if crosshair is tracking, convert the event position to RAS
    // (takes into account what lightbox the cursor is in)
    double xyz[4];
    this->ConvertDeviceToXYZ(pos[0], pos[1], xyz);
    double ras[4];
    this->Internal->GetSliceNode()->GetXYToRAS()->MultiplyPoint(xyz, ras);
    ras[0] /= ras[3]; ras[1] /= ras[3]; ras[2] /= ras[3]; ras[3] /= ras[3];

    // set the new position on the crosshair
    this->Internal->CrosshairNode->SetCrosshairRAS(ras);
    }
}

//---------------------------------------------------------------------------
int vtkMRMLCrosshairDisplayableManager::ActiveInteractionModes()
{
  // Crosshairs wants to get events all the time
  return vtkMRMLInteractionNode::Place | vtkMRMLInteractionNode::ViewTransform;
}


//---------------------------------------------------------------------------
void vtkMRMLCrosshairDisplayableManager::AdditionalInitializeStep()
{
  // Add interactor style styles to watch
  this->AddInteractorStyleObservableEvent(vtkCommand::MouseMoveEvent);

  // Build the initial crosshair representation
  this->Internal->BuildCrosshair();
}


//---------------------------------------------------------------------------
void vtkMRMLCrosshairDisplayableManager::SetLightBoxRendererManagerProxy(vtkMRMLLightBoxRendererManagerProxy* mgr)
{
  this->Internal->LightBoxRendererManagerProxy = mgr;
}