/* MollenOS
*
* Copyright 2011 - 2016, Philip Meulengracht
*
* This program is free software : you can redistribute it and / or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation ? , either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.If not, see <http://www.gnu.org/licenses/>.
*
*
* MollenOS - Generic List
*/

/* Includes */
#include <ds/list.h>
#include <stddef.h>
#include <string.h>

/* Instantiates a new list
 * with the given attribs and keytype */
List_t *ListCreate(KeyType_t KeyType, int Attributes)
{
	/* Allocate a new instance */
	List_t *List = (List_t*)dsalloc(sizeof(List_t));
	memset(List, 0, sizeof(List_t));

	/* Set initial stuff, especially
	 * set the data key type */
	List->Attributes = Attributes;
	List->KeyType = KeyType;

	/* Do we use a lock? */
	if (Attributes & LIST_SAFE)
		SpinlockReset(&List->Lock);

	/* Done! */
	return List;
}

/* Destroys the list and
 * frees all resources associated
 * does also free all list elements
 * and keys */
void ListDestroy(List_t *List)
{
	/* Vars */
	ListNode_t *Node = ListPopFront(List);

	/* Keep freeing nodes */
	while (Node != NULL) 
	{
		/* Free node, and next! */
		ListDestroyNode(List, Node);
		Node = ListPopFront(List);
	}

	/* Free list */
	dsfree(List);
}

/* Returns the length of the
 * given list */
int ListLength(List_t *List)
{
	/* Vars */
	int RetVal = 0;

	/* Sanity */
	if (List == NULL)
		return 0;

	/* Get lock */
	if (List->Attributes & LIST_SAFE)
		SpinlockAcquire(&List->Lock);

	/* Store in temporary
	 * var while we have lock */
	RetVal = List->Length;

	/* Release Lock */
	if (List->Attributes & LIST_SAFE)
		SpinlockRelease(&List->Lock);

	/* Done! */
	return RetVal;
}

/* Instantiates a new list node
 * that can be appended to the list
 * by ListAppend. If using an unsorted list
 * set the sortkey == key */
ListNode_t *ListCreateNode(DataKey_t Key, DataKey_t SortKey, void *Data)
{
	/* Allocate a new node */
	ListNode_t *Node = (ListNode_t*)dsalloc(sizeof(ListNode_t));
	memset(Node, 0, sizeof(ListNode_t));

	/* Set items */
	Node->Key = Key;
	Node->SortKey = SortKey;
	Node->Data = Data;

	/* Done! */
	return Node;
}

/* Cleans up a list node and frees
 * all resources it had */
void ListDestroyNode(List_t *List, ListNode_t *Node)
{
	/* Destroy key */
	switch (List->KeyType)
	{
		case KeyPointer:
		case KeyString:
			dsfree(Node->Key.Pointer);
			break;

		default:
			break;
	}

	/* Destroy sort key */
	switch (List->KeyType)
	{
		case KeyPointer:
		case KeyString:
			if (Node->Key.Pointer != Node->SortKey.Pointer)
				dsfree(Node->SortKey.Pointer);
			break;

		default:
			break;
	}

	/* Destroy node */
	dsfree(Node);
}

/* Insert the node into a specific position
 * in the list, if position is invalid it is
 * inserted at the back. This function is not
 * available for sorted lists, it will simply
 * call ListInsert instead */
void ListInsertAt(List_t *List, ListNode_t *Node, int Position)
{
	/* Sanity */
	if (List == NULL || Node == NULL)
		return;

	/* Redirect if we are using a insert-sorted
	 * list, as we cannot insert at specific pos */
	if (List->Attributes & LIST_SORT_ONINSERT) {
		ListInsert(List, Node);
		return;
	}

	/* TODO */
	_CRT_UNUSED(Position);
}

/* Inserts the node into the front of
 * the list. This should be used for sorted
 * lists, but is available for unsorted lists
 * aswell */
void ListInsert(List_t *List, ListNode_t *Node)
{
	/* Sanity */
	if (List == NULL || Node == NULL)
		return;

	/* Get lock */
	if (List->Attributes & LIST_SAFE)
		SpinlockAcquire(&List->Lock);

	/* So, do we need to do an insertion sort? */
	if (List->Attributes & LIST_SORT_ONINSERT) {
		/* Empty list  ? */
		if (List->Headp == NULL || List->Tailp == NULL) {
			List->Tailp = List->Headp = Node;
			
			/* Set link NULL (EoL) */
			Node->Link = NULL;
		}
		else
		{
			/* Keep track of insertion */
			int Inserted = 0;

			/* Iterate till we find our spot */
			foreach(lNode, List)
			{
				/* Let's see */
				if (dssortkey(List->KeyType, Node->SortKey, lNode->SortKey) == 1) {
					/* We're bigger, continue */
				}
				else {
					/* Ok, so we need to be inserted before lNode */
					if (lNode->Prev == NULL) {
						/* Make the node point to head */
						Node->Link = lNode;
						lNode->Prev = Node;

						/* Make the node the new head */
						List->Headp = Node;
					}
					else {
						/* Insert between nodes 
						 * lNode->Prev <---> Node <---> lNode */

						/* lNode->Prev ---> Node */
						lNode->Prev->Link = Node;

						/* lNode->Prev <--- Node */
						Node->Prev = lNode->Prev;

						/* Node <---- lNode */
						lNode->Prev = Node;

						/* Node ----> lNode */
						Node->Link = lNode;
					}

					/* Update inserted */
					Inserted = 1;
					break;
				}
			}

			/* Sanity 
			 * If inserted is 0, append us to 
			 * back of list */
			if (!Inserted) {
				/* Update current tail link */
				List->Tailp->Link = Node;

				/* Now make tail point to this */
				Node->Prev = List->Tailp;
				List->Tailp = Node;

				/* Set link NULL (EoL) */
				Node->Link = NULL;
			}
		}
	}
	else {
		/* Empty list  ? */
		if (List->Headp == NULL || List->Tailp == NULL) {
			List->Tailp = List->Headp = Node;
			Node->Link = NULL;
		}
		else
		{
			/* Make the node point to head */
			Node->Link = List->Headp;
			List->Headp->Prev = Node;

			/* Make the node the new head */
			List->Headp = Node;
		}

		/* Set previous NONE */
		Node->Prev = NULL;
	}

	/* Increase Count */
	List->Length++;

	/* Release Lock */
	if (List->Attributes & LIST_SAFE)
		SpinlockRelease(&List->Lock);
}

/* Inserts the node into the the back
 * of the list. This function is not
 * available for sorted lists, it will
 * simply redirect to ListInsert */
void ListAppend(List_t *List, ListNode_t *Node)
{
	/* Sanity */
	if (List == NULL || Node == NULL)
		return;

	/* Redirect if we are using a insert-sorted
	* list, as we cannot insert at specific pos */
	if (List->Attributes & LIST_SORT_ONINSERT) {
		ListInsert(List, Node);
		return;
	}

	/* Get lock */
	if (List->Attributes & LIST_SAFE)
		SpinlockAcquire(&List->Lock);

	/* Empty list ? */
	if (List->Headp == NULL || List->Tailp == NULL) {
		Node->Prev = NULL;
		List->Tailp = List->Headp = Node;
	}
	else {
		/* Update current tail link */
		List->Tailp->Link = Node;

		/* Now make tail point to this */
		Node->Prev = List->Tailp;
		List->Tailp = Node;
	}

	/* Set link NULL (EoL) */
	Node->Link = NULL;

	/* Increase Count */
	List->Length++;

	/* Release Lock */
	if (List->Attributes & LIST_SAFE)
		SpinlockRelease(&List->Lock);
}

/* List pop functions, the either
 * remove an element from the back or
 * the front of the given list and return
 * the node */
ListNode_t *ListPopFront(List_t *List)
{
	/* Manipulate the list to find the next pointer of the
	 * node that comes before the one to be removed. */
	ListNode_t *Current = NULL;

	/* Sanity */
	if (List == NULL)
		return NULL;

	/* Get lock */
	if (List->Attributes & LIST_SAFE)
		SpinlockAcquire(&List->Lock);

	/* Sanity */
	if (List->Headp != NULL)
	{
		/* Get the head */
		Current = List->Headp;

		/* Update head pointer */
		List->Headp = Current->Link;

		/* Set previous to null */
		if (List->Headp != NULL)
			List->Headp->Prev = NULL;

		/* Update tail if necessary */
		if (List->Tailp == Current)
			List->Headp = List->Tailp = NULL;

		/* Reset its link (remove any list traces!) */
		Current->Link = NULL;
		Current->Prev = NULL;

		/* Update length */
		List->Length--;
	}

	/* Release Lock */
	if (List->Attributes & LIST_SAFE)
		SpinlockRelease(&List->Lock);

	/* Return */
	return Current;
}

/* List pop functions, the either
 * remove an element from the back or
 * the front of the given list and return
 * the node */
ListNode_t *ListPopBack(List_t *List)
{
	/* TODO */
	_CRT_UNUSED(List);
	return NULL;
}

/* These are the index-retriever functions
* they return the given index by either
* Key, data or node, return -1 if not found */
int ListGetIndexByData(List_t *List, void *Data)
{
	/* TODO */
	_CRT_UNUSED(List);
	_CRT_UNUSED(Data);
	return -1;
}

int ListGetIndexByKey(List_t *List, DataKey_t Key)
{
	/* TODO */
	_CRT_UNUSED(List);
	_CRT_UNUSED(Key);
	return -1;
}

int ListGetIndexByNode(List_t *List, ListNode_t *Node)
{
	/* TODO */
	_CRT_UNUSED(List);
	_CRT_UNUSED(Node);
	return -1;
}

/* These are the node-retriever functions
 * they return the list-node by either key
 * data or index */
ListNode_t *ListGetNodeByKey(List_t *List, DataKey_t Key, int n)
{
	/* Variables */
	ListNode_t *i, *found = NULL;
	int Counter = n;

	/* Sanity */
	if (List == NULL || List->Headp == NULL || List->Length == 0)
		return NULL;

	/* Get lock */
	if (List->Attributes & LIST_SAFE)
		SpinlockAcquire(&List->Lock);

	/* Iterate each member in the 
	 * given list */
	_foreach(i, List)
	{
		if (!dsmatchkey(List->KeyType, i->Key, Key))
		{
			if (Counter == 0) {
				found = i;
				break;
			}
			else
				Counter--;
		}
	}

	/* Release Lock */
	if (List->Attributes & LIST_SAFE)
		SpinlockRelease(&List->Lock);

	/* If we reach here, not enough of id */
	return found;
}

/* These are the data-retriever functions
 * they return the list-node by either key
 * node or index */
void *ListGetDataByKey(List_t *List, DataKey_t Key, int n)
{
	/* Reuse */
	ListNode_t *Node = ListGetNodeByKey(List, Key, n);

	/* Sanity */
	if (Node != NULL)
		return Node->Data;
	else
		return NULL;
}

/* These functions execute a given function
 * on all relevant nodes (see names) */
void ListExecuteOnKey(List_t *List, void(*Function)(void*, int, void*), DataKey_t Key, void *UserData)
{
	/* Variables */
	ListNode_t *Node;
	int Itr = 0;

	/* Sanity */
	if (List == NULL || List->Headp == NULL || List->Length == 0)
		return;

	_foreach(Node, List)
	{
		/* Check */
		if (!dsmatchkey(List->KeyType, Node->Key, Key))
		{
			/* Execute */
			Function(Node->Data, Itr, UserData);

			/* Increase */
			Itr++;
		}
	}
}

/* These functions execute a given function
 * on all relevant nodes (see names) */
void ListExecuteAll(List_t *List, void(*Function)(void*, int, void*), void *UserData)
{
	/* Variables */
	ListNode_t *Node;
	int Itr = 0;

	/* Sanity */
	if (List == NULL || List->Headp == NULL || List->Length == 0)
		return;

	_foreach(Node, List)
	{
		/* Execute */
		Function(Node->Data, Itr, UserData);

		/* Increase */
		Itr++;
	}
}

/* This functions unlinks a node
 * and returns the next node for
 * usage */
ListNode_t *ListUnlinkNode(List_t *List, ListNode_t *Node)
{
	/* Sanity */
	if (List == NULL 
		|| List->Headp == NULL 
		|| Node == NULL)
		return NULL;

	/* Get lock */
	if (List->Attributes & LIST_SAFE)
		SpinlockAcquire(&List->Lock);

	/* There are a few cases we need to handle
	 * in order for this to be O(1) */
	if (Node->Prev == NULL) {
		/* Ok, so this means we are the
		 * first node in the list
		 * Do we have a link? */
		if (Node->Link == NULL) {
			/* We're the only link
			 * but lets stil validate
			 * we're from this list */
			if (List->Headp == Node) {
				List->Headp = List->Tailp = NULL;
				List->Length--;
			}
		}
		else {
			/* We have a link
			 * this means we set headp to next */
			if (List->Headp == Node) {
				/* Set head pointer to next */
				List->Headp = Node->Link;

				/* Update previous */
				List->Headp->Prev = NULL;

				/* Reduce length */
				List->Length--;
			}
		}
	}
	else {
		/* We have a previous,
		 * Special case 1: we are last element
		 * which means we should update pointer */
		if (Node->Link == NULL) {
			/* Ok, we are last element */
			if (List->Tailp == Node) {
				/* Update tail pointer to previous */
				List->Tailp = Node->Prev;

				/* Update link to EOL */
				List->Tailp->Link = NULL;

				/* Reduce length */
				List->Length--;
			}
		}
		else {
			/* Normal case, we just skip this
			 * element without interfering with the list
			 * pointers */
			ListNode_t *Prev = Node->Prev;

			/* Update previous to -> link */
			Prev->Link = Node->Link;

			/* Update link previous to -> prev */
			Prev->Link->Prev = Prev;

			/* Reduce list */
			List->Length--;
		}
	}

	/* Release Lock */
	if (List->Attributes & LIST_SAFE)
		SpinlockRelease(&List->Lock);

	/* Does node have a next? */
	if (Node->Prev == NULL) {
		return List->Headp;
	}
	else {
		return Node->Link;
	}
}

/* These are the deletion functions
 * and remove based on either node
 * index or key */
void ListRemoveByNode(List_t *List, ListNode_t* Node)
{
	/* Reuse the unlink */
	ListUnlinkNode(List, Node);

	/* Update links */
	Node->Link = NULL;
	Node->Prev = NULL;
}

/* These are the deletion functions
 * and remove based on either node
 * index or key */
void ListRemoveByIndex(List_t *List, int Index)
{
	/* TODO */
	_CRT_UNUSED(List);
	_CRT_UNUSED(Index);
}

/* These are the deletion functions
 * and remove based on either node
 * index or key */
int ListRemoveByKey(List_t *List, DataKey_t Key)
{
	/* Step 1, lookup node */
	ListNode_t *Node = ListGetNodeByKey(List, Key, 0);

	/* Step 2, remove it */
	if (Node != NULL) {
		ListRemoveByNode(List, Node);
		ListDestroyNode(List, Node);
		return 1;
	}

	/* Damn */
	return 0;
}